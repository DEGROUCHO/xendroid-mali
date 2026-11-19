/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_XEX2_UTIL_H_
#define XENIA_KERNEL_UTIL_XEX2_UTIL_H_

#include <cstddef>
#include <cstdint>

#include "xenia/kernel/util/xex2_info.h"

namespace xe {

// Special case: if key & 0xFF == 0x00, returns the value in *out_ptr as
// uint32_t, not a pointer.
inline bool GetXexOptHeader(const xex2_header* header, xex2_header_keys key,
                            void** out_ptr) {
  if (!header || !out_ptr) {
    return false;
  }

  for (uint32_t i = 0; i < header->header_count; i++) {
    const xex2_opt_header& opt_header = header->headers[i];
    if (opt_header.key == key) {
      switch (key & 0xFF) {
        case 0x00:
          *reinterpret_cast<uint32_t*>(out_ptr) =
              static_cast<uint32_t>(opt_header.value);
          break;
        case 0x01:
          *out_ptr = const_cast<void*>(
              reinterpret_cast<const void*>(&opt_header.value));
          break;
        default:
          *out_ptr =
              reinterpret_cast<void*>(uintptr_t(header) + opt_header.offset);
          break;
      }
      return true;
    }
  }
  return false;
}

template <typename T>
inline bool GetXexOptHeader(const xex2_header* header, xex2_header_keys key,
                            T* out_ptr) {
  return GetXexOptHeader(header, key, reinterpret_cast<void**>(out_ptr));
}

// Why the first `length` bytes of a XEX do not hold its whole header, or nullptr
// when they do: header_size inside the file, and inside header_size the optional
// header directory, the data of each entry, and the security info with its page
// descriptors. GetXexOptHeader and the loader read all of these straight from the
// file, trusting its counts and offsets.
inline const char* XexHeaderProblem(const void* data, size_t length) {
  constexpr size_t kFixedSize = offsetof(xex2_header, headers);
  if (!data || length < kFixedSize) {
    return "the file is shorter than a XEX header";
  }
  const auto* bytes = static_cast<const uint8_t*>(data);
  const auto* header = static_cast<const xex2_header*>(data);
  const uint64_t header_size = header->header_size;
  if (header_size < kFixedSize || header_size > length) {
    return "the header size is outside the file";
  }
  const uint32_t count = header->header_count;
  if (kFixedSize + uint64_t(count) * sizeof(xex2_opt_header) > header_size) {
    return "the optional header list is outside the header";
  }
  for (uint32_t i = 0; i < count; i++) {
    // Keys ending in 0x00 or 0x01 keep their data in the entry itself. The
    // others point at (key & 0xFF) dwords, or for 0xFF at a block whose first
    // dword is its size in bytes.
    const uint32_t dwords = header->headers[i].key & 0xFF;
    if (dwords <= 0x01) {
      continue;
    }
    const uint64_t offset = header->headers[i].offset;
    uint64_t size = uint64_t(dwords) * 4;
    if (dwords == 0xFF) {
      if (offset + 4 > header_size) {
        return "an optional header is outside the header";
      }
      size = *reinterpret_cast<const xe::be<uint32_t>*>(bytes + offset);
      if (size < 4) {
        return "an optional header is shorter than its size field";
      }
    }
    if (offset + size > header_size) {
      return "an optional header is outside the header";
    }
  }
  const uint64_t security = header->security_offset;
  uint64_t descriptors;
  uint32_t pages;
  if (header->magic == 0x58455831) {  // 'XEX1'
    descriptors = security + offsetof(xex1_security_info, page_descriptors);
    if (descriptors > header_size) {
      return "the security info is outside the header";
    }
    pages = reinterpret_cast<const xex1_security_info*>(bytes + security)
                ->page_descriptor_count;
  } else {
    descriptors = security + offsetof(xex2_security_info, page_descriptors);
    if (descriptors > header_size) {
      return "the security info is outside the header";
    }
    pages = reinterpret_cast<const xex2_security_info*>(bytes + security)
                ->page_descriptor_count;
  }
  if (descriptors + uint64_t(pages) * sizeof(xex2_page_descriptor) >
      header_size) {
    return "the page descriptors are outside the header";
  }
  return nullptr;
}

}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_XEX2_UTIL_H_
