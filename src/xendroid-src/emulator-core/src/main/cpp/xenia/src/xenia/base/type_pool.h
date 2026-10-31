/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_TYPE_POOL_H_
#define XENIA_BASE_TYPE_POOL_H_

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace xe {

template <class T, typename A>
class TypePool {
 public:
  ~TypePool() { Reset(); }

  void Reset() {
    std::lock_guard<std::mutex> guard(lock_);
    for (auto it = list_.begin(); it != list_.end(); ++it) {
      T* value = *it;
      delete value;
    }
    list_.clear();
  }

  T* Allocate(A arg0) {
    T* result = 0;
    {
      std::lock_guard<std::mutex> guard(lock_);
      if (list_.size()) {
        result = list_.back();
        list_.pop_back();
      }
    }
    if (!result) {
      result = new T(arg0);
    }
    return result;
  }

  // Values beyond max_idle idle ones are destroyed instead of kept.
  void Release(T* value, size_t max_idle = SIZE_MAX) {
    {
      std::lock_guard<std::mutex> guard(lock_);
      if (list_.size() < max_idle) {
        list_.push_back(value);
        return;
      }
    }
    delete value;
  }

 private:
  std::mutex lock_;
  std::vector<T*> list_;
};

}  // namespace xe

#endif  // XENIA_BASE_TYPE_POOL_H_
