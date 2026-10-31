// XenDroid runtime snapshot container. Payload providers must quiesce all writers.
#ifndef XENIA_SNAPSHOT_STATE_FILE_H_
#define XENIA_SNAPSHOT_STATE_FILE_H_
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/auxv.h>
#endif
namespace xe::snapshot {
constexpr uint32_t kFormatVersion = 1;
constexpr uint64_t kMaxFileBytes = uint64_t{2} << 30;
constexpr uint32_t kSectionCount = 6;
enum class Section : uint32_t { kCpu=1, kMemory, kKernel, kGpu, kAudio, kClock };
struct Identity {
  uint32_t title_id = 0;
  uint32_t guest_page_size = 0;
  std::array<uint8_t,32> build_hash{};
  std::array<uint8_t,32> media_hash{};
  std::array<uint8_t,32> config_hash{};
  bool operator==(const Identity&) const = default;
};
struct Image { Identity identity; std::array<std::vector<uint8_t>,kSectionCount> sections; };
// CRC-32 (zlib polynomial). Snapshots are hundreds of MB: the ARMv8 CRC32
// instructions (or 8-way table slicing) instead of a bit loop per byte.
namespace detail {
inline const std::array<std::array<uint32_t,256>,8>& Crc32Tables() {
  static const auto tables=[] {
    std::array<std::array<uint32_t,256>,8> t{};
    for (uint32_t i=0;i<256;++i) {
      uint32_t c=i;
      for (unsigned k=0;k<8;++k) c=(c>>1)^(0xEDB88320u & (0u-(c&1)));
      t[0][i]=c;
    }
    for (uint32_t i=0;i<256;++i)
      for (unsigned k=1;k<8;++k) t[k][i]=(t[k-1][i]>>8)^t[0][t[k-1][i]&0xFF];
    return t;
  }();
  return tables;
}
inline uint32_t Crc32Sliced(uint32_t crc,const uint8_t* p,size_t n) {
  const auto& t=Crc32Tables();
  while (n>=8) {
    uint32_t lo=crc^(uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24));
    uint32_t hi=uint32_t(p[4])|(uint32_t(p[5])<<8)|(uint32_t(p[6])<<16)|(uint32_t(p[7])<<24);
    crc=t[7][lo&0xFF]^t[6][(lo>>8)&0xFF]^t[5][(lo>>16)&0xFF]^t[4][lo>>24]^
        t[3][hi&0xFF]^t[2][(hi>>8)&0xFF]^t[1][(hi>>16)&0xFF]^t[0][hi>>24];
    p+=8; n-=8;
  }
  while (n--) crc=(crc>>8)^t[0][(crc^*p++)&0xFF];
  return crc;
}
#if defined(__aarch64__) && (defined(__clang__) || defined(__GNUC__))
__attribute__((target("crc"))) inline uint32_t Crc32Arm(uint32_t crc,const uint8_t* p,size_t n) {
  while (n && (reinterpret_cast<uintptr_t>(p)&7)) { crc=__builtin_arm_crc32b(crc,*p++); --n; }
  while (n>=8) { uint64_t v; std::memcpy(&v,p,8); crc=__builtin_arm_crc32d(crc,v); p+=8; n-=8; }
  while (n--) crc=__builtin_arm_crc32b(crc,*p++);
  return crc;
}
inline bool HasArmCrc32() {
#if defined(__APPLE__)
  return true;
#elif defined(__linux__) || defined(__ANDROID__)
  static const bool has=(getauxval(AT_HWCAP)&(1ul<<7))!=0;  // HWCAP_CRC32
  return has;
#else
  return false;
#endif
}
#endif
}  // namespace detail
// Running CRC: start from ~0u, finish with ~state.
inline uint32_t Crc32Update(uint32_t state,std::span<const uint8_t> bytes) {
#if defined(__aarch64__) && (defined(__clang__) || defined(__GNUC__))
  if (detail::HasArmCrc32()) return detail::Crc32Arm(state,bytes.data(),bytes.size());
#endif
  return detail::Crc32Sliced(state,bytes.data(),bytes.size());
}
inline uint32_t Crc32(std::span<const uint8_t> bytes) {
  return ~Crc32Update(~uint32_t{0},bytes);
}
inline void Put32(std::vector<uint8_t>& out,uint32_t v) {
  for(unsigned i=0;i<4;++i) out.push_back(uint8_t(v>>(i*8)));
}
inline void Put64(std::vector<uint8_t>& out,uint64_t v) {
  Put32(out,uint32_t(v)); Put32(out,uint32_t(v>>32));
}
class Reader {
 public:
  explicit Reader(std::span<const uint8_t> bytes):bytes_(bytes){}
  bool Bytes(std::span<uint8_t> dst) {
    if(dst.size()>bytes_.size()-offset_) return false;
    if(!dst.empty()) std::memcpy(dst.data(),bytes_.data()+offset_,dst.size());
    offset_+=dst.size(); return true;
  }
  bool U32(uint32_t& v) { std::array<uint8_t,4> b{}; if(!Bytes(b))return false;
    v=uint32_t(b[0])|(uint32_t(b[1])<<8)|(uint32_t(b[2])<<16)|(uint32_t(b[3])<<24); return true; }
  bool U64(uint64_t& v) { uint32_t lo,hi; if(!U32(lo)||!U32(hi))return false; v=lo|(uint64_t(hi)<<32);return true; }
  bool Skip(size_t count) { if(count>remaining())return false;offset_+=count;return true; }
  size_t remaining() const {return bytes_.size()-offset_;}
  size_t offset() const {return offset_;}
 private: std::span<const uint8_t> bytes_; size_t offset_=0;
};
inline bool Encode(const Image& image,std::vector<uint8_t>& out,std::string& error) {
  uint64_t total=8+4+4+4+4+96+4+kSectionCount*(4+8+4);
  for(const auto& part:image.sections) {
    if(part.empty()||part.size()>kMaxFileBytes-total) {error="missing or oversized snapshot section";return false;}
    total+=part.size();
  }
  std::vector<uint8_t> data; data.reserve(size_t(total));
  const std::array<uint8_t,8> magic{'X','D','S','T','A','T','E',0}; data.insert(data.end(),magic.begin(),magic.end());
  Put32(data,kFormatVersion); Put32(data,0x01020304); // explicit little endian schema marker
  Put32(data,image.identity.title_id); Put32(data,image.identity.guest_page_size);
  for(const auto* hash:{&image.identity.build_hash,&image.identity.media_hash,&image.identity.config_hash}) data.insert(data.end(),hash->begin(),hash->end());
  Put32(data,kSectionCount);
  for(uint32_t i=0;i<kSectionCount;++i) {Put32(data,i+1);Put64(data,image.sections[i].size());Put32(data,Crc32(image.sections[i]));}
  for(const auto& part:image.sections) data.insert(data.end(),part.begin(),part.end());
  out=std::move(data); return true;
}
// Validate the complete file before publishing parsed bytes or touching runtime.
inline bool Decode(std::span<const uint8_t> bytes,const Identity& expected,Image& out,std::string& error) {
  if(bytes.size()>kMaxFileBytes) {error="snapshot exceeds limit";return false;}
  Reader r(bytes); std::array<uint8_t,8> magic{}; uint32_t version,endian,count; Image candidate;
  if(!r.Bytes(magic)||magic!=std::array<uint8_t,8>{'X','D','S','T','A','T','E',0}||!r.U32(version)||version!=kFormatVersion||!r.U32(endian)||endian!=0x01020304) {error="legacy or incompatible snapshot format";return false;}
  if(!r.U32(candidate.identity.title_id)||!r.U32(candidate.identity.guest_page_size)||!r.Bytes(candidate.identity.build_hash)||!r.Bytes(candidate.identity.media_hash)||!r.Bytes(candidate.identity.config_hash)||!r.U32(count)||count!=kSectionCount) {error="truncated snapshot header";return false;}
  if(candidate.identity!=expected) {error="snapshot title/build/media/config mismatch";return false;}
  std::array<uint64_t,kSectionCount> lengths{}; std::array<uint32_t,kSectionCount> checksums{};
  uint64_t payload=0;
  for(uint32_t i=0;i<kSectionCount;++i) {uint32_t id;
    if(!r.U32(id)||id!=i+1||!r.U64(lengths[i])||!r.U32(checksums[i])||lengths[i]==0||lengths[i]>kMaxFileBytes-payload) {error="invalid snapshot section table";return false;}
    payload+=lengths[i];
  }
  if(payload!=r.remaining()) {error="truncated or trailing snapshot payload";return false;}
  // Check integrity before allocating/copying sections.
  size_t cursor=r.offset();
  for(uint32_t i=0;i<kSectionCount;++i) {auto part=bytes.subspan(cursor,size_t(lengths[i])); if(Crc32(part)!=checksums[i]) {error="snapshot checksum mismatch";return false;} cursor+=part.size();}
  for(uint32_t i=0;i<kSectionCount;++i) {candidate.sections[i].resize(size_t(lengths[i])); if(!r.Bytes(candidate.sections[i])) {error="snapshot section read failed";return false;}}
  out=std::move(candidate); return true;
}
// mkstemp ensures concurrent or failed saves never truncate the previous slot.
// Rename is the commit point. After rename, a directory-fsync error is reported
// as a durability warning separately: the valid new file is already published.
inline bool AtomicWrite(const std::filesystem::path& path,std::span<const uint8_t> data,std::string& error,std::string* durability_warning=nullptr) {
  if(data.empty()||data.size()>kMaxFileBytes) {error="invalid snapshot size";return false;}
  std::string pattern=path.string()+".tmp.XXXXXX";
  std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);
  int fd=::mkstemp(name.data()); if(fd<0){error="create snapshot temp failed";return false;}
  // mkstemp creates 0600: let the user copy or back up the slot (adb, file manager).
  ::fchmod(fd,0660);
  bool ok=true;size_t offset=0;
  while(offset<data.size()) {ssize_t n=::write(fd,data.data()+offset,data.size()-offset);if(n<0&&errno==EINTR)continue;if(n<=0){ok=false;break;}offset+=size_t(n);}
  if(ok&&::fsync(fd)!=0)ok=false;
  if(::close(fd)!=0)ok=false;
  if(ok&&::rename(name.data(),path.c_str())!=0)ok=false;
  if(!ok){::unlink(name.data());error="snapshot write/flush/commit failed";return false;}
  auto parent=path.parent_path();if(parent.empty())parent=".";
  int dir=::open(parent.c_str(),O_RDONLY|O_DIRECTORY);
  if(dir<0){if(durability_warning)*durability_warning="snapshot committed; directory open failed";}
  else {if(::fsync(dir)!=0&&durability_warning)*durability_warning="snapshot committed; directory fsync failed";::close(dir);}
  return true;
}
inline bool ReadFile(const std::filesystem::path& path,std::vector<uint8_t>& out,std::string& error) {
  int fd=::open(path.c_str(),O_RDONLY);if(fd<0){error="open snapshot failed";return false;}
  off_t length=::lseek(fd,0,SEEK_END);
  if(length<=0||uint64_t(length)>kMaxFileBytes||::lseek(fd,0,SEEK_SET)<0){::close(fd);error="invalid snapshot file size";return false;}
  std::vector<uint8_t> bytes(static_cast<size_t>(length));size_t offset=0;
  while(offset<bytes.size()){ssize_t n=::read(fd,bytes.data()+offset,bytes.size()-offset);if(n<0&&errno==EINTR)continue;if(n<=0){::close(fd);error="short snapshot read";return false;}offset+=size_t(n);}
  // Detect concurrent file growth. AtomicWrite publishes a different inode,
  // so a normal concurrent save leaves this fd stable.
  uint8_t extra;ssize_t n;do{n=::read(fd,&extra,1);}while(n<0&&errno==EINTR);
  ::close(fd);if(n!=0){error="snapshot changed during read";return false;}
  out=std::move(bytes);return true;
}
} // namespace xe::snapshot
#endif
