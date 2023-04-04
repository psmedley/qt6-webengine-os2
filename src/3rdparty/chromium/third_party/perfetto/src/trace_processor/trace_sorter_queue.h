/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACE_PROCESSOR_TRACE_SORTER_QUEUE_H_
#define SRC_TRACE_PROCESSOR_TRACE_SORTER_QUEUE_H_

#include <cstddef>
#include <deque>
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/trace_sorter_internal.h"

namespace perfetto {
namespace trace_processor {
namespace trace_sorter_internal {

// 1MB is good tradeoff between having big enough memory blocks so that we don't
// need to often append and remove blocks for big traces, but small enough to
// not overuse memory for small traces.
static constexpr uint32_t kDefaultSize = 1 * 1024 * 1024;  // 1MB

// Used for storing the data for all different packet data types.
class VariadicQueue {
 public:
  VariadicQueue() : VariadicQueue(kDefaultSize) {}
  ~VariadicQueue() {
    // These checks verify that we evicted all elements from this queue. This is
    // important as we need to call the destructor to make sure we're not
    // leaking memory.
    FreeMemory();
    PERFETTO_CHECK(mem_blocks_.size() == 1);
    PERFETTO_CHECK(mem_blocks_.back().empty());
  }

  VariadicQueue(const VariadicQueue&) = delete;
  VariadicQueue& operator=(const VariadicQueue&) = delete;

  VariadicQueue(VariadicQueue&&) = default;
  VariadicQueue& operator=(VariadicQueue&&) noexcept = default;

  // Moves packet data type to the end of the queue storage.
  template <typename T>
  uint32_t Append(T value) {
    PERFETTO_DCHECK(!mem_blocks_.empty());

    uint64_t size = Block::AppendSize<T>(value);
    if (PERFETTO_UNLIKELY(!mem_blocks_.back().HasSpace(size))) {
      mem_blocks_.emplace_back(Block(block_size_));
    }
    auto& back_block = mem_blocks_.back();
    PERFETTO_DCHECK(back_block.HasSpace(size));
    return GlobalMemOffsetFromLastBlockOffset(
        back_block.Append(std::move(value)));
  }

  // Moves object out of queue storage.
  template <typename T>
  T Evict(uint32_t global_offset) {
    uint32_t block = (global_offset / block_size_) - deleted_blocks_;
    uint32_t block_offset = global_offset % block_size_;
    return mem_blocks_[block].Evict<T>(block_offset);
  }

  // Clears the empty front of queue storage.
  void FreeMemory() {
    while (mem_blocks_.size() > 1 && mem_blocks_.front().empty()) {
      mem_blocks_.pop_front();
      deleted_blocks_++;
    }
  }

  // Returns the offset value in which new element can be stored.
  uint32_t NextOffset() const {
    PERFETTO_DCHECK(!mem_blocks_.empty());
    return GlobalMemOffsetFromLastBlockOffset(mem_blocks_.back().offset());
  }

  static VariadicQueue VariadicQueueForTesting(uint32_t size) {
    return VariadicQueue(size);
  }

 private:
  // Implementation note: this class stores an extra 8 bytes in debug builds to
  // store the size of the type stored inside.
  class Block {
   public:
    explicit Block(uint32_t block_size)
        : size_(block_size),
          storage_(
              base::AlignedAllocTyped<uint64_t>(size_ / sizeof(uint64_t))) {}

    bool HasSpace(uint64_t size) const { return size <= size_ - offset_; }

    template <typename T>
    uint32_t Append(T value) {
      static_assert(alignof(T) <= 8,
                    "Class must have at most 8 byte alignment");
      PERFETTO_DCHECK(offset_ % 8 == 0);
      PERFETTO_DCHECK(HasSpace(AppendSize(value)));

      char* storage_begin_ptr = reinterpret_cast<char*>(storage_.get());
      char* ptr = storage_begin_ptr + offset_;

#if PERFETTO_DCHECK_IS_ON()
      ptr = AppendUnchecked(ptr, TypedMemoryAccessor<T>::AppendSize(value));
#endif
      ptr = TypedMemoryAccessor<T>::Append(ptr, std::move(value));
      num_elements_++;

      auto cur_offset = offset_;
      offset_ = static_cast<uint32_t>(base::AlignUp<8>(static_cast<uint32_t>(
          ptr - reinterpret_cast<char*>(storage_.get()))));
      return cur_offset;
    }

    template <typename T>
    T Evict(uint32_t offset) {
      PERFETTO_DCHECK(offset < size_);
      PERFETTO_DCHECK(offset % 8 == 0);

      char* ptr = reinterpret_cast<char*>(storage_.get()) + offset;
      uint64_t size = 0;
#if PERFETTO_DCHECK_IS_ON()
      size = EvictUnchecked<uint64_t>(&ptr);
#endif
      T value = TypedMemoryAccessor<T>::Evict(ptr);
      PERFETTO_DCHECK(size == TypedMemoryAccessor<T>::AppendSize(value));
      num_elements_evicted_++;
      return value;
    }

    template <typename T>
    static uint64_t AppendSize(const T& value) {
#if PERFETTO_DCHECK_IS_ON()
      // On debug runs for each append of T we also append the sizeof(T) to the
      // queue for sanity check, which we later evict and compare with object
      // size. This value needs to be added to general size of an object.
      return sizeof(uint64_t) + TypedMemoryAccessor<T>::AppendSize(value);
#else
      return TypedMemoryAccessor<T>::AppendSize(value);
#endif
    }

    uint32_t offset() const { return offset_; }
    bool empty() const { return num_elements_ == num_elements_evicted_; }

   private:
    uint32_t size_;
    uint32_t offset_ = 0;

    uint32_t num_elements_ = 0;
    uint32_t num_elements_evicted_ = 0;

    base::AlignedUniquePtr<uint64_t> storage_;
  };

  explicit VariadicQueue(uint32_t block_size) : block_size_(block_size) {
    mem_blocks_.emplace_back(Block(block_size_));
  }

  uint32_t GlobalMemOffsetFromLastBlockOffset(uint32_t block_offset) const {
    return (deleted_blocks_ + static_cast<uint32_t>(mem_blocks_.size()) - 1) *
               block_size_ +
           block_offset;
  }

  std::deque<Block> mem_blocks_;

  uint32_t block_size_ = kDefaultSize;
  uint32_t deleted_blocks_ = 0;
};

}  // namespace trace_sorter_internal
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACE_SORTER_QUEUE_H_
