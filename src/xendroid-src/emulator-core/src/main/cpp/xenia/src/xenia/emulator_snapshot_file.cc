// Runtime snapshot file I/O, format 2 (see snapshot/state_stream.h).
//
// Layout: the format 1 header (magic, version, endian marker, identity,
// section count), then one table entry per section
//   u32 id, u32 codec, u64 raw length, u64 stored length, u32 CRC-32 of the
//   stored bytes
// and the stored sections in order. The table is written last, in place. A
// zstd section is one or more concatenated frames.
#include "xenia/snapshot/state_stream.h"

#include <algorithm>
#include <atomic>
#include <new>
#include <system_error>

#include "third_party/zstd/lib/zstd.h"

namespace xe::snapshot {
namespace {
constexpr size_t kHeaderBytes = 8 + 4 + 4 + 4 + 4 + 32 * 3 + 4;
constexpr size_t kEntryBytes = 4 + 4 + 8 + 8 + 4;
constexpr size_t kCompressMinBytes = size_t{64} << 10;
constexpr std::array<uint8_t, 8> kMagic{'X', 'D', 'S', 'T', 'A', 'T', 'E', 0};

bool WriteAll(int fd, const uint8_t* data, size_t size) {
  while (size) {
    ssize_t n = ::write(fd, data, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    data += n;
    size -= size_t(n);
  }
  return true;
}

bool PwriteAll(int fd, const uint8_t* data, size_t size, off_t offset) {
  while (size) {
    ssize_t n = ::pwrite(fd, data, size, offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    data += n;
    size -= size_t(n);
    offset += n;
  }
  return true;
}

bool ReadAll(int fd, uint8_t* data, size_t size) {
  while (size) {
    ssize_t n = ::read(fd, data, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    data += n;
    size -= size_t(n);
  }
  return true;
}

struct FdCloser {
  int fd = -1;
  ~FdCloser() {
    if (fd >= 0) ::close(fd);
  }
};

struct CCtxDeleter {
  void operator()(ZSTD_CCtx* c) const { ZSTD_freeCCtx(c); }
};
struct DCtxDeleter {
  void operator()(ZSTD_DCtx* d) const { ZSTD_freeDCtx(d); }
};

std::unique_ptr<ZSTD_CCtx, CCtxDeleter> MakeCCtx(int level) {
  std::unique_ptr<ZSTD_CCtx, CCtxDeleter> cctx(ZSTD_createCCtx());
  if (cctx && ZSTD_isError(ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_compressionLevel,
                                                  level))) {
    cctx.reset();
  }
  return cctx;
}

enum class FrameDecodeResult { kDone, kUnknownSize, kError };

struct JoiningThreads {
  std::array<std::thread, 4> threads;
  ~JoiningThreads() {
    for (auto& thread : threads) {
      if (thread.joinable()) thread.join();
    }
  }
};

FrameDecodeResult DecompressMemoryFrames(std::span<const uint8_t> compressed,
                                        std::span<uint8_t> dst,
                                        std::string& error) {
  struct Frame {
    size_t stored_offset, stored_size, raw_offset, raw_size;
  };
  std::vector<Frame> frames;
  size_t stored_offset = 0, raw_offset = 0;
  while (stored_offset < compressed.size()) {
    const auto* src = compressed.data() + stored_offset;
    const size_t remaining = compressed.size() - stored_offset;
    const size_t stored_size = ZSTD_findFrameCompressedSize(src, remaining);
    if (ZSTD_isError(stored_size) || !stored_size || stored_size > remaining) {
      error = "snapshot section decompression failed";
      return FrameDecodeResult::kError;
    }
    const auto raw_size = ZSTD_getFrameContentSize(src, stored_size);
    if (raw_size == ZSTD_CONTENTSIZE_ERROR) {
      error = "snapshot section decompression failed";
      return FrameDecodeResult::kError;
    }
    if (raw_size == ZSTD_CONTENTSIZE_UNKNOWN) {
      return FrameDecodeResult::kUnknownSize;
    }
    // Subtraction bounds both the output offset and the size_t conversion.
    if (raw_size > dst.size() - raw_offset) {
      error = "snapshot section size mismatch";
      return FrameDecodeResult::kError;
    }
    frames.push_back({stored_offset, stored_size, raw_offset, size_t(raw_size)});
    stored_offset += stored_size;
    raw_offset += size_t(raw_size);
  }
  if (frames.empty() || raw_offset != dst.size()) {
    error = "snapshot section size mismatch";
    return FrameDecodeResult::kError;
  }

  const unsigned worker_count = unsigned(std::min<size_t>(
      frames.size(),
      std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u)));
  std::array<std::unique_ptr<ZSTD_DCtx, DCtxDeleter>, 4> contexts;
  for (unsigned i = 0; i < worker_count; ++i) {
    contexts[i].reset(ZSTD_createDCtx());
    if (!contexts[i]) {
      error = "snapshot decompressor setup failed";
      return FrameDecodeResult::kError;
    }
  }
  std::atomic<size_t> next{0};
  std::atomic<bool> failed{false};
  {
    // Join on every exit, including exceptions from std::thread construction.
    JoiningThreads workers;
    try {
      for (unsigned i = 0; i < worker_count; ++i) {
        workers.threads[i] = std::thread([&, i] {
          while (!failed.load(std::memory_order_relaxed)) {
            const size_t index = next.fetch_add(1, std::memory_order_relaxed);
            if (index >= frames.size()) return;
            const auto& frame = frames[index];
            const size_t result = ZSTD_decompressDCtx(
                contexts[i].get(), dst.data() + frame.raw_offset, frame.raw_size,
                compressed.data() + frame.stored_offset, frame.stored_size);
            if (ZSTD_isError(result) || result != frame.raw_size) {
              failed.store(true, std::memory_order_relaxed);
              return;
            }
          }
        });
      }
    } catch (const std::system_error&) {
      failed.store(true, std::memory_order_relaxed);
      error = "snapshot decompressor thread setup failed";
      return FrameDecodeResult::kError;
    }
  }
  if (failed.load(std::memory_order_relaxed)) {
    error = "snapshot section decompression failed";
    return FrameDecodeResult::kError;
  }
  return FrameDecodeResult::kDone;
}
}  // namespace

struct ImageFileWriter::Job {
  enum class State { kPending, kRunning, kDone };
  std::vector<uint8_t> in;
  std::vector<uint8_t> out;
  size_t out_size = 0;
  State state = State::kPending;
  bool ok = true;
};

ImageFileWriter::ImageFileWriter() = default;

ImageFileWriter::~ImageFileWriter() {
  StopWorkers();
  Abort();
}

void ImageFileWriter::Abort() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (!temp_name_.empty()) {
    ::unlink(temp_name_.data());
    temp_name_.clear();
  }
}

bool ImageFileWriter::Begin(const std::filesystem::path& path, const Identity& identity,
                            int zstd_level, std::string& error) {
  if (fd_ >= 0) {
    error = "snapshot writer already open";
    return false;
  }
  path_ = path;
  level_ = zstd_level;
  std::string pattern = path.string() + ".tmp.XXXXXX";
  temp_name_.assign(pattern.begin(), pattern.end());
  temp_name_.push_back(0);
  fd_ = ::mkstemp(temp_name_.data());
  if (fd_ < 0) {
    temp_name_.clear();
    error = "create snapshot temp failed";
    return false;
  }
  // mkstemp creates 0600: let the user copy or back up the slot.
  ::fchmod(fd_, 0660);
  std::vector<uint8_t> head(kMagic.begin(), kMagic.end());
  Put32(head, kStreamFormatVersion);
  Put32(head, 0x01020304);
  Put32(head, identity.title_id);
  Put32(head, identity.guest_page_size);
  for (const auto* hash : {&identity.build_hash, &identity.media_hash,
                           &identity.config_hash}) {
    head.insert(head.end(), hash->begin(), hash->end());
  }
  Put32(head, kSectionCount);
  // Placeholder table, rewritten by Commit once the stored lengths are known.
  head.resize(kHeaderBytes + kSectionCount * kEntryBytes, 0);
  if (!WriteAll(fd_, head.data(), head.size())) {
    Abort();
    error = "snapshot write failed";
    return false;
  }
  next_section_ = 0;
  table_.clear();
  totals_ = {};
  return true;
}

bool ImageFileWriter::WriteBytes(const uint8_t* data, size_t size, uint32_t& crc,
                                 uint64_t& stored) {
  if (!WriteAll(fd_, data, size)) return false;
  crc = Crc32Update(crc, std::span<const uint8_t>(data, size));
  stored += size;
  return true;
}

bool ImageFileWriter::WriteSection(uint32_t index, std::span<const uint8_t> data,
                                   std::string& error) {
  if (fd_ < 0 || streaming_ || index != next_section_ || index >= kSectionCount) {
    error = "snapshot sections written out of order";
    return false;
  }
  if (data.empty() || data.size() > kMaxSectionRawBytes) {
    error = "missing or oversized snapshot section";
    return false;
  }
  uint32_t crc = ~uint32_t{0};
  uint64_t stored = 0;
  uint32_t codec = kCodecRaw;
  bool ok;
  if (data.size() >= kCompressMinBytes) {
    codec = kCodecZstd;
    auto cctx = MakeCCtx(level_);
    std::vector<uint8_t> out(ZSTD_compressBound(data.size()));
    size_t n = cctx ? ZSTD_compress2(cctx.get(), out.data(), out.size(), data.data(),
                                     data.size())
                    : size_t(-1);
    ok = cctx && !ZSTD_isError(n) && WriteBytes(out.data(), n, crc, stored);
  } else {
    ok = WriteBytes(data.data(), data.size(), crc, stored);
  }
  if (!ok) {
    Abort();
    error = "snapshot compression or write failed";
    return false;
  }
  Put32(table_, index + 1);
  Put32(table_, codec);
  Put64(table_, data.size());
  Put64(table_, stored);
  Put32(table_, ~crc);
  totals_.raw_bytes += data.size();
  totals_.stored_bytes += stored;
  ++next_section_;
  return true;
}

bool ImageFileWriter::BeginStreamSection(uint32_t index, unsigned workers,
                                         std::string& error) {
  if (fd_ < 0 || streaming_ || index != next_section_ || index >= kSectionCount) {
    error = "snapshot sections written out of order";
    return false;
  }
  workers = std::clamp(workers, 1u, 8u);
  streaming_ = true;
  stream_failed_ = false;
  stream_crc_ = ~uint32_t{0};
  stream_raw_ = stream_stored_ = 0;
  max_in_flight_ = size_t(workers) * 2;
  stop_ = false;
  for (unsigned i = 0; i < workers; ++i) {
    workers_.emplace_back([this] { Worker(); });
  }
  return true;
}

void ImageFileWriter::Worker() {
  auto cctx = MakeCCtx(level_);
  for (;;) {
    Job* job = nullptr;
    {
      std::unique_lock lock(mutex_);
      work_cv_.wait(lock, [this] {
        if (stop_) return true;
        for (auto& j : jobs_) {
          if (j->state == Job::State::kPending) return true;
        }
        return false;
      });
      for (auto& j : jobs_) {
        if (j->state == Job::State::kPending) {
          job = j.get();
          break;
        }
      }
      if (!job) return;
      job->state = Job::State::kRunning;
    }
    bool ok = bool(cctx);
    if (ok) {
      job->out.resize(ZSTD_compressBound(job->in.size()));
      size_t n = ZSTD_compress2(cctx.get(), job->out.data(), job->out.size(),
                                job->in.data(), job->in.size());
      ok = !ZSTD_isError(n);
      job->out_size = ok ? n : 0;
    }
    // Release the raw chunk as soon as it is compressed.
    std::vector<uint8_t>().swap(job->in);
    {
      std::lock_guard lock(mutex_);
      job->ok = ok;
      job->state = Job::State::kDone;
    }
    done_cv_.notify_all();
  }
}

bool ImageFileWriter::WriteOldest() {
  std::unique_ptr<Job> job;
  {
    std::unique_lock lock(mutex_);
    if (jobs_.empty()) return true;
    done_cv_.wait(lock, [this] { return jobs_.front()->state == Job::State::kDone; });
    job = std::move(jobs_.front());
    jobs_.pop_front();
  }
  return job->ok && WriteBytes(job->out.data(), job->out_size, stream_crc_,
                               stream_stored_);
}

bool ImageFileWriter::Submit() {
  if (!filling_ || filling_->in.empty()) return true;
  for (;;) {
    size_t in_flight;
    {
      std::lock_guard lock(mutex_);
      in_flight = jobs_.size();
    }
    if (in_flight < max_in_flight_) break;
    if (!WriteOldest()) return false;
  }
  {
    std::lock_guard lock(mutex_);
    jobs_.push_back(std::move(filling_));
  }
  work_cv_.notify_one();
  return true;
}

bool ImageFileWriter::Append(const uint8_t* data, size_t size) {
  if (!streaming_ || stream_failed_) return false;
  while (size) {
    if (!filling_) {
      filling_ = std::make_unique<Job>();
      filling_->in.reserve(chunk_bytes_);
    }
    const size_t n = std::min(chunk_bytes_ - filling_->in.size(), size);
    filling_->in.insert(filling_->in.end(), data, data + n);
    data += n;
    size -= n;
    stream_raw_ += n;
    if (stream_raw_ > kMaxSectionRawBytes ||
        (filling_->in.size() == chunk_bytes_ && !Submit())) {
      stream_failed_ = true;
      return false;
    }
  }
  return true;
}

void ImageFileWriter::StopWorkers() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    // Work nobody will write any more.
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                               [](const std::unique_ptr<Job>& j) {
                                 return j->state == Job::State::kPending;
                               }),
                jobs_.end());
  }
  work_cv_.notify_all();
  for (auto& worker : workers_) worker.join();
  workers_.clear();
  jobs_.clear();
  filling_.reset();
}

bool ImageFileWriter::EndStreamSection(std::string& error) {
  if (!streaming_) {
    error = "snapshot stream section not open";
    return false;
  }
  bool ok = !stream_failed_ && Submit();
  while (ok) {
    bool empty;
    {
      std::lock_guard lock(mutex_);
      empty = jobs_.empty();
    }
    if (empty) break;
    ok = WriteOldest();
  }
  StopWorkers();
  streaming_ = false;
  if (!ok || !stream_raw_) {
    Abort();
    error = "snapshot compression or write failed";
    return false;
  }
  Put32(table_, next_section_ + 1);
  Put32(table_, kCodecZstd);
  Put64(table_, stream_raw_);
  Put64(table_, stream_stored_);
  Put32(table_, ~stream_crc_);
  totals_.raw_bytes += stream_raw_;
  totals_.stored_bytes += stream_stored_;
  ++next_section_;
  return true;
}

bool ImageFileWriter::Commit(bool durable, FileStats* stats, std::string& error) {
  if (fd_ < 0 || streaming_ || next_section_ != kSectionCount ||
      table_.size() != kSectionCount * kEntryBytes) {
    Abort();
    error = "snapshot file incomplete";
    return false;
  }
  if (totals_.stored_bytes > kMaxFileBytes - kHeaderBytes - table_.size()) {
    Abort();
    error = "snapshot exceeds limit";
    return false;
  }
  if (!PwriteAll(fd_, table_.data(), table_.size(), off_t(kHeaderBytes)) ||
      (durable && ::fsync(fd_) != 0)) {
    Abort();
    error = "snapshot write/flush failed";
    return false;
  }
  const int fd = fd_;
  fd_ = -1;
  if (::close(fd) != 0 || ::rename(temp_name_.data(), path_.c_str()) != 0) {
    ::unlink(temp_name_.data());
    temp_name_.clear();
    error = "snapshot commit failed";
    return false;
  }
  temp_name_.clear();
  if (durable) {
    auto parent = path_.parent_path();
    if (parent.empty()) parent = ".";
    int dir = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir >= 0) {
      ::fsync(dir);
      ::close(dir);
    }
  }
  if (stats) *stats = totals_;
  return true;
}

bool WriteImageFile(const std::filesystem::path& path, const Image& image,
                    int zstd_level, FileStats* stats, std::string& error) {
  ImageFileWriter writer;
  if (!writer.Begin(path, image.identity, zstd_level, error)) return false;
  for (uint32_t i = 0; i < kSectionCount; ++i) {
    if (!writer.WriteSection(i, image.sections[i], error)) return false;
  }
  return writer.Commit(true, stats, error);
}

bool ReadImageFile(const std::filesystem::path& path, const Identity& expected,
                   Image& out, FileStats* stats, std::string& error) {
  FdCloser file;
  file.fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (file.fd < 0) {
    error = "no saved state for this game";
    return false;
  }
  struct stat st;
  if (::fstat(file.fd, &st) != 0 || st.st_size <= 0 ||
      uint64_t(st.st_size) > kMaxFileBytes) {
    error = "invalid snapshot file size";
    return false;
  }
  const uint64_t file_size = uint64_t(st.st_size);
  std::array<uint8_t, kHeaderBytes> head;
  if (file_size < head.size() || !ReadAll(file.fd, head.data(), head.size())) {
    error = "truncated snapshot header";
    return false;
  }
  Reader r(head);
  std::array<uint8_t, 8> magic;
  uint32_t version = 0, endian = 0, count = 0;
  Identity identity;
  if (!r.Bytes(magic) || magic != kMagic || !r.U32(version)) {
    error = "legacy or incompatible snapshot format";
    return false;
  }
  if (version == kFormatVersion) {
    // Format 1: one uncompressed image, decoded in memory.
    std::vector<uint8_t> bytes;
    Image legacy;
    if (!ReadFile(path, bytes, error) || !Decode(bytes, expected, legacy, error)) {
      return false;
    }
    if (stats) {
      stats->raw_bytes = stats->stored_bytes = bytes.size();
    }
    out = std::move(legacy);
    return true;
  }
  if (version != kStreamFormatVersion || !r.U32(endian) || endian != 0x01020304 ||
      !r.U32(identity.title_id) || !r.U32(identity.guest_page_size) ||
      !r.Bytes(identity.build_hash) || !r.Bytes(identity.media_hash) ||
      !r.Bytes(identity.config_hash) || !r.U32(count) || count != kSectionCount) {
    error = "legacy or incompatible snapshot format";
    return false;
  }
  if (identity != expected) {
    error = "snapshot title/build/media/config mismatch";
    return false;
  }
  std::array<uint8_t, kSectionCount * kEntryBytes> table_bytes;
  if (!ReadAll(file.fd, table_bytes.data(), table_bytes.size())) {
    error = "invalid snapshot section table";
    return false;
  }
  struct Entry {
    uint32_t codec;
    uint64_t raw;
    uint64_t stored;
    uint32_t crc;
  };
  std::array<Entry, kSectionCount> entries;
  Reader t(table_bytes);
  uint64_t stored_total = 0, raw_total = 0;
  for (uint32_t i = 0; i < kSectionCount; ++i) {
    uint32_t id;
    auto& e = entries[i];
    if (!t.U32(id) || id != i + 1 || !t.U32(e.codec) || !t.U64(e.raw) ||
        !t.U64(e.stored) || !t.U32(e.crc) ||
        (e.codec != kCodecRaw && e.codec != kCodecZstd) || !e.raw ||
        e.raw > kMaxSectionRawBytes || !e.stored ||
        (e.codec == kCodecRaw && e.stored != e.raw) ||
        e.stored > file_size - stored_total) {
      error = "invalid snapshot section table";
      return false;
    }
    stored_total += e.stored;
    raw_total += e.raw;
  }
  if (raw_total > kMaxSectionRawBytes ||
      stored_total != file_size - head.size() - table_bytes.size()) {
    error = "truncated or trailing snapshot payload";
    return false;
  }

  Image candidate;
  candidate.identity = identity;
  std::unique_ptr<ZSTD_DCtx, DCtxDeleter> dctx(ZSTD_createDCtx());
  if (!dctx) {
    error = "snapshot decompressor setup failed";
    return false;
  }
  std::vector<uint8_t> in_buffer(size_t{1} << 20);
  try {
    for (uint32_t i = 0; i < kSectionCount; ++i) {
      const auto& e = entries[i];
      auto& dst = candidate.sections[i];
      dst.resize(size_t(e.raw));
      uint32_t crc = ~uint32_t{0};
      if (e.codec == kCodecRaw) {
        if (!ReadAll(file.fd, dst.data(), dst.size())) {
          error = "short snapshot read";
          return false;
        }
        crc = Crc32Update(crc, dst);
      } else {
        std::vector<uint8_t> compressed;
        if (i + 1 == uint32_t(Section::kMemory)) {
          compressed.resize(size_t(e.stored));
          if (!ReadAll(file.fd, compressed.data(), compressed.size())) {
            error = "short snapshot read";
            return false;
          }
          crc = Crc32Update(crc, compressed);
          if (~crc != e.crc) {
            error = "snapshot checksum mismatch";
            return false;
          }
          const auto result = DecompressMemoryFrames(compressed, dst, error);
          if (result == FrameDecodeResult::kError) return false;
          if (result == FrameDecodeResult::kDone) continue;
          // An unknown frame size uses the existing streaming decoder below.
        }
        if (ZSTD_isError(ZSTD_DCtx_reset(dctx.get(), ZSTD_reset_session_only))) {
          error = "snapshot decompressor setup failed";
          return false;
        }
        uint64_t left = e.stored;
        size_t produced = 0;
        bool frame_done = false;
        while (left) {
          const size_t n = size_t(std::min<uint64_t>(in_buffer.size(), left));
          if (compressed.empty()) {
            if (!ReadAll(file.fd, in_buffer.data(), n)) {
              error = "short snapshot read";
              return false;
            }
            crc = Crc32Update(crc, std::span<const uint8_t>(in_buffer.data(), n));
          } else {
            // Memory's compressed bytes and CRC have already been read.
            std::memcpy(in_buffer.data(),
                        compressed.data() + size_t(e.stored - left), n);
          }
          left -= n;
          ZSTD_inBuffer in{in_buffer.data(), n, 0};
          while (in.pos < in.size) {
            ZSTD_outBuffer o{dst.data(), dst.size(), produced};
            const size_t before_in = in.pos, before_out = o.pos;
            const size_t result = ZSTD_decompressStream(dctx.get(), &o, &in);
            if (ZSTD_isError(result)) {
              error = "snapshot section decompression failed";
              return false;
            }
            produced = o.pos;
            // 0 ends a frame; another frame may follow in the same section.
            frame_done = result == 0;
            if (!frame_done && in.pos == before_in && o.pos == before_out) {
              // Output full and no progress: more data than declared.
              error = "snapshot section size mismatch";
              return false;
            }
          }
        }
        if (!frame_done || produced != dst.size()) {
          error = "snapshot section size mismatch";
          return false;
        }
      }
      if (~crc != e.crc) {
        error = "snapshot checksum mismatch";
        return false;
      }
    }
  } catch (const std::bad_alloc&) {
    error = "not enough memory to load the snapshot";
    return false;
  }
  uint8_t extra;
  ssize_t n;
  do {
    n = ::read(file.fd, &extra, 1);
  } while (n < 0 && errno == EINTR);
  if (n != 0) {
    error = "snapshot changed during read";
    return false;
  }
  if (stats) {
    stats->raw_bytes = raw_total;
    stats->stored_bytes = stored_total;
  }
  out = std::move(candidate);
  return true;
}
}  // namespace xe::snapshot
