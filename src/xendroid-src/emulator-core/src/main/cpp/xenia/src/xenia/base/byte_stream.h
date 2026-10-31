/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_BYTE_STREAM_H_
#define XENIA_BASE_BYTE_STREAM_H_

#include <cstdint>
#include <string>
#include <type_traits>

namespace xe {

class ByteStream {
 public:
  ByteStream(uint8_t* data, size_t data_length, size_t offset = 0);
  ~ByteStream();

  void Advance(size_t num_bytes);
  void Read(uint8_t* buf, size_t len);
  void Write(const uint8_t* buf, size_t len);

  void Read(void* buf, size_t len) {
    return Read(reinterpret_cast<uint8_t*>(buf), len);
  }
  void Write(const void* buf, size_t len) {
    return Write(reinterpret_cast<const uint8_t*>(buf), len);
  }

  const uint8_t* data() const { return data_; }
  uint8_t* data() { return data_; }
  size_t data_length() const { return data_length_; }

  size_t offset() const { return offset_; }
  void set_offset(size_t offset) { offset_ = offset; }

  // True once any Read/Write/Advance ran past the end of the buffer.
  bool failed() const { return failed_; }

  template <typename T>
  T Read() {
    T data;
    Read(reinterpret_cast<uint8_t*>(&data), sizeof(T));

    return data;
  }

  template <typename T>
  void Write(T data) {
    // Raw bytes only: a std::string here would store its pointers instead of
    // its characters (the overloads below take strings).
    static_assert(std::is_trivially_copyable_v<T>,
                  "ByteStream::Write(T) copies raw bytes");
    Write(reinterpret_cast<uint8_t*>(&data), sizeof(T));
  }

  // Length-prefixed, matching Read<std::string>(). The std::string overloads
  // are needed: for a std::string argument the raw-bytes template above is an
  // exact match and would win over the string_view conversion.
  void Write(const std::string_view str) {
    Write(uint32_t(str.length()));
    Write(str.data(), str.length() * sizeof(char));
  }
  void Write(const std::string& str) { Write(std::string_view(str)); }
  void Write(const char* str) { Write(std::string_view(str)); }

  void Write(const std::u16string_view str) {
    Write(uint32_t(str.length()));
    Write(str.data(), str.length() * sizeof(char16_t));
  }
  void Write(const std::u16string& str) { Write(std::u16string_view(str)); }

 private:
  uint8_t* data_ = nullptr;
  size_t data_length_ = 0;
  size_t offset_ = 0;
  bool failed_ = false;
};

template <>
std::string ByteStream::Read();

template <>
std::u16string ByteStream::Read();

}  // namespace xe

#endif  // XENIA_BASE_BYTE_STREAM_H_
