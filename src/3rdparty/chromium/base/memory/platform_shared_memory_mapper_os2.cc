// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/memory/platform_shared_memory_mapper.h"

#include "base/logging.h"
#include "base/numerics/safe_conversions.h"

#include <libcx/shmem.h>

namespace base {

// static
absl::optional<span<uint8_t>> PlatformSharedMemoryMapper::Map(
    subtle::PlatformSharedMemoryHandle handle,
    bool write_allowed,
    uint64_t offset,
    size_t size) {
  void* address;
  address = shmem_map(handle, offset, size);
  if (!address) {
    DPLOG(ERROR) << "shmem_mmap(" << handle << ") failed";
    return absl::nullopt;
  }

  return make_span(reinterpret_cast<uint8_t*>(address),
                   size);
}

// static
void PlatformSharedMemoryMapper::Unmap(span<uint8_t> mapping) {
  if (shmem_unmap(mapping.data()) == -1)
    DPLOG(ERROR) << "shmem_unmap";
}

}  // namespace base
