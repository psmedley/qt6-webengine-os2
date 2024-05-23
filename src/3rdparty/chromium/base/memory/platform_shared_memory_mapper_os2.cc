// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/memory/platform_shared_memory_mapper.h"

#include "base/logging.h"
#include "base/numerics/safe_conversions.h"

#include <libcx/shmem.h>

namespace base {

// static
bool PlatformSharedMemoryMapper::MapInternal(
    subtle::PlatformSharedMemoryHandle handle,
    bool write_allowed,
    uint64_t offset,
    size_t size,
    void** memory,
    size_t* mapped_size) {

  *memory = shmem_map(handle, offset, size);
  if (!*memory) {
    DPLOG(ERROR) << "shmem_mmap(" << handle << ") failed";
    return false;
  }

  *mapped_size = size;
  return true;
}

// static
void PlatformSharedMemoryMapper::UnmapInternal(void* memory, size_t size) {
  if (shmem_unmap(memory) == -1)
    DPLOG(ERROR) << "shmem_unmap";
}

}  // namespace base

