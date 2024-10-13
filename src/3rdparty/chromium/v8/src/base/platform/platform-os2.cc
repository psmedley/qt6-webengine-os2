// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define INCL_BASE
#define INCL_EXAPIS
#include <os2.h>
#include <libcx/shmem.h>
#include <cstdlib>

#include "src/base/macros.h"
#include "src/base/platform/platform-posix-time.h"
#include "src/base/platform/platform-posix.h"
#include "src/base/platform/platform.h"

namespace v8 {
namespace base {

namespace {

ULONG GetProtectionFromMemoryPermission(OS::MemoryPermission access) {
  switch (access) {
    case OS::MemoryPermission::kNoAccess:
    case OS::MemoryPermission::kNoAccessWillJitLater:
      return 0;  // no permissions
    case OS::MemoryPermission::kRead:
      return PAG_READ;
    case OS::MemoryPermission::kReadWrite:
      return PAG_READ | PAG_WRITE;
    case OS::MemoryPermission::kReadWriteExecute:
      return PAG_READ | PAG_WRITE | PAG_EXECUTE;
    case OS::MemoryPermission::kReadExecute:
      return PAG_READ | PAG_EXECUTE;
  }
  UNREACHABLE();
}

}  // namespace

TimezoneCache* OS::CreateTimezoneCache() {
  return new PosixDefaultTimezoneCache();
}

// static
void* OS::Allocate(void* address, size_t size, size_t alignment,
                   OS::MemoryPermission access) {
  size_t page_size = OS::AllocatePageSize();
  DCHECK_EQ(0, size % page_size);
  DCHECK_EQ(0, alignment % page_size);
  address = AlignedAddress(address, alignment);

  ULONG flags = GetProtectionFromMemoryPermission(access);
  if (flags == 0) // kNoAccess
    flags = PAG_READ; // OS/2 requires at least one permission bit.
  else
    flags |= PAG_COMMIT;
  if (address)
    flags |= OBJ_LOCATION;
  else
    flags |= OBJ_ANY;

  // Since this method is relaxed and must return at least something, we try
  // things in the following order:
  // 1. Allocate with OBJ_LOCATION if address is not nullptr.
  // 2. Fall back to any address with OBJ_ANY if OBJ_LOCATION fails.
  // 3. Fall back to low mem if OBJ_ANY (high mem) fails.

  void* base = address;
  APIRET arc = DosAllocMemEx(&base, size, flags);
  if (arc != NO_ERROR && (flags & (OBJ_LOCATION | OBJ_ANY))) {
    if (flags & OBJ_LOCATION) {
      flags &= ~OBJ_LOCATION;
      flags |= OBJ_ANY;
      arc = DosAllocMemEx(&base, size, flags);
    }
    if (arc != NO_ERROR) {
      flags &= ~OBJ_ANY;
      arc = DosAllocMemEx(&base, size, flags);
    }
  }
  if (arc != NO_ERROR) return nullptr;  // Can't allocate, we're OOM.

  // If address is suitably aligned, we're done.
  void* aligned_base = reinterpret_cast<void*>(
      RoundUp(reinterpret_cast<uintptr_t>(base), alignment));
  if (base == aligned_base) return base;

  // Otherwise, free it and try a larger allocation.
  CHECK_EQ(NO_ERROR, DosFreeMem(base));

  // Clear the hint. It's unlikely we can allocate at this address.
  address = nullptr;
  flags &= ~OBJ_ANY;

  // Add the maximum misalignment so we are guaranteed an aligned base address
  // in the allocated region.
  size_t padded_size = size + (alignment - page_size);
  const int kMaxAttempts = 3;
  aligned_base = nullptr;
  for (int i = 0; i < kMaxAttempts; ++i) {
    arc = DosAllocMemEx(&base, padded_size, flags | OBJ_ANY);
    if (arc != NO_ERROR)
      arc = DosAllocMemEx(&base, padded_size, flags);
    if (arc != NO_ERROR) return nullptr;  // Can't allocate, we're OOM.

    // Try to trim the allocation by freeing the padded allocation and then
    // calling VirtualAlloc at the aligned base.
    CHECK_EQ(NO_ERROR, DosFreeMem(base));
    aligned_base = reinterpret_cast<void*>(
        RoundUp(reinterpret_cast<uintptr_t>(base), alignment));
    base = aligned_base;
    arc = DosAllocMemEx(&base, size, flags | OBJ_LOCATION | OBJ_ANY);
    if (arc != NO_ERROR)
      arc = DosAllocMemEx(&base, padded_size, flags | OBJ_LOCATION);
    // We might not get the reduced allocation due to a race. In that case,
    // arc will be set to some error.
    if (arc == NO_ERROR) break;
  }
  DCHECK_IMPLIES(base, base == aligned_base);
  return base;
}

void* OS::AllocateShared(void* hint, size_t size, MemoryPermission access,
                         PlatformSharedMemoryHandle handle, uint64_t offset) {

  void* result = shmem_map(handle, offset, size);
  if (result) {
    return nullptr;
  }
  return result;
}

// static
void OS::FreeShared(void* address, size_t size) {
  DCHECK_EQ(0, size % AllocatePageSize());
  CHECK_EQ(0, shmem_unmap(address));
}

// static
void OS::Free(void* address, const size_t size) {
  DCHECK_EQ(0, reinterpret_cast<uintptr_t>(address) % AllocatePageSize());
  DCHECK_EQ(0, size % AllocatePageSize());
  USE(size);
  CHECK_EQ(NO_ERROR, DosFreeMemEx(address));
}

// static
void OS::Release(void* address, size_t size) {
  DCHECK_EQ(0, reinterpret_cast<uintptr_t>(address) % CommitPageSize());
  DCHECK_EQ(0, size % CommitPageSize());
  CHECK_EQ(NO_ERROR, DosSetMem(address, size, PAG_DECOMMIT));
}

// static
bool OS::SetPermissions(void* address, size_t size, MemoryPermission access) {
  DCHECK_EQ(0, reinterpret_cast<uintptr_t>(address) % CommitPageSize());
  DCHECK_EQ(0, size % CommitPageSize());
  ULONG flags = GetProtectionFromMemoryPermission(access);
  // Query real page flags as OS/2 doesn't let committing committed pages
  // and decommitting decommitted ones.
  ULONG realSize = size, realFlags = 0;
  APIRET arc = DosQueryMem(address, &realSize, &realFlags);
  if (arc == NO_ERROR) {
    if (flags == 0) { // kNoAcess
      if (realFlags & PAG_COMMIT)
        arc = DosSetMem(address, size, PAG_DECOMMIT);
    } else {
      if (!(realFlags & PAG_COMMIT))
        flags |= PAG_COMMIT;
      arc = DosSetMem(address, size, flags);
    }
  }
  return arc == NO_ERROR;
}

bool OS::DiscardSystemPages(void* address, size_t size) {
  // OS/2 does not seem to support this function.
  return true;
}

// static
bool OS::DecommitPages(void* address, size_t size) {
  return SetPermissions(address, size, MemoryPermission::kNoAccess);
}

// static
bool OS::CanReserveAddressSpace() {
  // TODO: Implement on OS/2.
  return false;
}

// static
bool AddressSpaceReservation::AllocateShared(void* address, size_t size,
                                             OS::MemoryPermission access,
                                             PlatformSharedMemoryHandle handle,
                                             uint64_t offset) {
  DCHECK(Contains(address, size));
  return shmem_map(handle, offset, size);
}

// static
Optional<AddressSpaceReservation> OS::CreateAddressSpaceReservation(
    void* hint, size_t size, size_t alignment,
    MemoryPermission max_permission) {
  CHECK(CanReserveAddressSpace());
  UNREACHABLE();  // TODO: Port to OS/2.
  return {};
}

// static
void OS::FreeAddressSpaceReservation(AddressSpaceReservation reservation) {
  UNREACHABLE();  // TODO: Port to OS/2.
}

// static
bool OS::HasLazyCommits() {
  // TODO: OS/2 doen't seem to have lazy commits.
  return false;
}

std::vector<OS::SharedLibraryAddress> OS::GetSharedLibraryAddresses() {
  UNREACHABLE();  // TODO: Port to OS/2.
}

void OS::SignalCodeMovingGC() {
  // Nothing to do on OS/2.
}

void OS::AdjustSchedulingParams() {}

// static
Stack::StackSlot Stack::GetStackStart() {
  PTIB ptib;
  if (DosGetInfoBlocks(&ptib, NULL))
    return nullptr;
  return ptib->tib_pstack;
}

}  // namespace base
}  // namespace v8
