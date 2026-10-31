// XenDroid runtime snapshot file I/O (format 2): sections are compressed and
// written straight to the file and read back into their final raw buffers.
// Restore additionally buffers the compressed memory section for parallel
// decoding of independent frames; unknown frame sizes use streaming decode.
#ifndef XENIA_SNAPSHOT_STATE_STREAM_H_
#define XENIA_SNAPSHOT_STATE_STREAM_H_
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "xenia/snapshot/state_file.h"
namespace xe::snapshot {
constexpr uint32_t kStreamFormatVersion = 2;
enum : uint32_t { kCodecRaw = 0, kCodecZstd = 1 };
// Upper bound for one decompressed section (guest RAM plus page tables).
constexpr uint64_t kMaxSectionRawBytes = uint64_t{3} << 30;
struct FileStats {
  uint64_t raw_bytes = 0;
  uint64_t stored_bytes = 0;
};

// Writes one snapshot file section by section, in section order. A section
// can also be streamed: its bytes arrive through Append() and are compressed
// in parallel as independent zstd frames, so guest RAM goes from the guest
// pages to the file through a few small buffers. Until Commit() the data is
// in a temporary file; destroying an uncommitted writer removes it.
class ImageFileWriter {
 public:
  ImageFileWriter();
  ImageFileWriter(const ImageFileWriter&) = delete;
  ImageFileWriter& operator=(const ImageFileWriter&) = delete;
  ~ImageFileWriter();

  bool Begin(const std::filesystem::path& path, const Identity& identity,
             int zstd_level, std::string& error);
  // |index| is the 0-based section index; sections are written in order.
  bool WriteSection(uint32_t index, std::span<const uint8_t> data,
                    std::string& error);
  bool BeginStreamSection(uint32_t index, unsigned workers, std::string& error);
  bool Append(const uint8_t* data, size_t size);
  bool EndStreamSection(std::string& error);
  // Writes the section table and publishes the file. |durable| also fsyncs
  // the file and its directory.
  bool Commit(bool durable, FileStats* stats, std::string& error);

 private:
  struct Job;
  bool WriteBytes(const uint8_t* data, size_t size, uint32_t& crc,
                  uint64_t& stored);
  bool Submit();
  bool WriteOldest();
  void Worker();
  void StopWorkers();
  void Abort();

  std::filesystem::path path_;
  std::vector<char> temp_name_;
  int fd_ = -1;
  int level_ = 1;
  uint32_t next_section_ = 0;
  std::vector<uint8_t> table_;
  FileStats totals_;
  // Streamed section state.
  bool streaming_ = false;
  bool stream_failed_ = false;
  uint32_t stream_crc_ = 0;
  uint64_t stream_raw_ = 0, stream_stored_ = 0;
  size_t chunk_bytes_ = size_t{4} << 20;
  size_t max_in_flight_ = 0;
  std::unique_ptr<Job> filling_;
  std::deque<std::unique_ptr<Job>> jobs_;
  std::mutex mutex_;
  std::condition_variable work_cv_, done_cv_;
  std::vector<std::thread> workers_;
  bool stop_ = false;
};

// Writes |image| atomically (temp file, fsync, rename).
bool WriteImageFile(const std::filesystem::path& path, const Image& image,
                    int zstd_level, FileStats* stats, std::string& error);
// Reads and verifies a format 2 file (or a legacy format 1 file) whose
// identity must equal |expected|. Memory frames with known content sizes are
// decoded by up to four workers; other sections use streaming I/O as before.
// On failure |out| is left unchanged.
bool ReadImageFile(const std::filesystem::path& path, const Identity& expected,
                   Image& out, FileStats* stats, std::string& error);
}  // namespace xe::snapshot
#endif
