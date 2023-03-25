// Copyright (c) 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_ALLOCATOR_PARTITION_ALLOCATOR_PAGE_ALLOCATOR_INTERNALS_OS2_H_
#define BASE_ALLOCATOR_PARTITION_ALLOCATOR_PAGE_ALLOCATOR_INTERNALS_OS2_H_

#include "base/allocator/partition_allocator/oom.h"
#include "base/allocator/partition_allocator/page_allocator_internal.h"
#include "base/logging.h"
#include "base/allocator/partition_allocator/partition_alloc_notreached.h"

namespace partition_alloc::internal {

namespace {

// Emulates semantics of Windows |VirtualAlloc|/|VirtualFree| that allow to
// commit and decommit already committed or decommitted pages.
APIRET MyDosSetMem(PVOID base, ULONG length, ULONG flags) {
  if (!(flags & (PAG_COMMIT | PAG_DECOMMIT)))
    return DosSetMem(base, length, flags);

  // Query the current state of each range to avoid committing/decommitting
  // already commited/decommitted pages.
  PVOID addr = base;
  APIRET arc;
  while (length) {
    ULONG act_len = length, act_flags, new_flags = flags;
    arc = DosQueryMem(addr, &act_len, &act_flags);
    if (arc != NO_ERROR)
      break;
    if ((new_flags & PAG_COMMIT) && (act_flags & PAG_COMMIT))
      new_flags &= ~PAG_COMMIT;
    if ((new_flags & PAG_DECOMMIT) && !(act_flags & (PAG_COMMIT | PAG_FREE)))
      new_flags &= ~PAG_DECOMMIT;
    if ((new_flags & (PAG_COMMIT | PAG_DECOMMIT)) ||
        (new_flags & fPERM) != (act_flags & fPERM)) {
      arc = DosSetMem(addr, act_len, new_flags);
      if (arc != NO_ERROR)
        break;
    }
    addr = static_cast<PVOID>(static_cast<char *>(addr) + act_len);
    length -= act_len;
  }

  return NO_ERROR;
}

} // namespace

// |DosAllocMemEx| will fail if allocation at the hint address is blocked.
constexpr bool kHintIsAdvisory = false;
std::atomic<int32_t> s_allocPageErrorCode{NO_ERROR};

int GetAccessFlags(PageAccessibilityConfiguration accessibility) {
  switch (accessibility) {
    case PageAccessibilityConfiguration::kRead:
      return PAG_READ;
    case PageAccessibilityConfiguration::kReadWrite:
      return PAG_READ | PAG_WRITE;
    case PageAccessibilityConfiguration::kReadExecute:
      return PAG_READ | PAG_EXECUTE;
    case PageAccessibilityConfiguration::kReadWriteExecute:
      return PAG_READ | PAG_WRITE | PAG_EXECUTE;
    default:
      PA_NOTREACHED();
      [[fallthrough]];
    case PageAccessibilityConfiguration::kInaccessible:
      return 0;
  }
}

uintptr_t SystemAllocPagesInternal(void* hint,
                               size_t length,
                               PageAccessibilityConfiguration accessibility,
                               PageTag page_tag) {
  ULONG flags = GetAccessFlags(accessibility);
  if (flags == 0)
    flags = PAG_READ; // OS/2 requires at least one permission bit.
  if (accessibility != PageAccessibilityConfiguration::kInaccessible)
    flags |= PAG_COMMIT;
  if (hint)
    flags |= OBJ_LOCATION;
  else
    flags |= OBJ_ANY; // Requiest high memory.

  void *base = hint;
  APIRET arc = DosAllocMemEx(&base, length, flags);
  if (arc != NO_ERROR && (flags & OBJ_ANY)) {
    // Try low memory.
    flags &= ~OBJ_ANY;
    arc = DosAllocMemEx(&base, length, flags);
  }
  if (arc != NO_ERROR) {
    s_allocPageErrorCode = arc;
    return 0;
  }
  return base;
}

uintptr_t TrimMappingInternal(uintptr_t base_address,
                          size_t base_length,
                          size_t trim_length,
                          PageAccessibilityConfiguration accessibility,
                          size_t pre_slack,
                          size_t post_slack) {
  uintptr_t ret = base_address;
  if (pre_slack || post_slack) {
    // We cannot resize the allocation run. Free it and retry at the aligned
    // address within the freed range.
    ret = base_address + pre_slack;
    FreePages(base_address, base_length);
    ret = SystemAllocPages(ret, trim_length, accessibility, PageTag::kChromium);
  }
  return ret;
}

bool TrySetSystemPagesAccessInternal(
    uintptr_t address,
    size_t length,
    PageAccessibilityConfiguration accessibility) {
  void* ptr = reinterpret_cast<void*>(address);
  if (accessibility == PageAccessibilityConfiguration::kInaccessible)
    return MyDosSetMem(ptr, length, PAG_DECOMMIT) == NO_ERROR;
  return MyDosSetMem(ptr, length, PAG_COMMIT |
                     GetAccessFlags(accessibility)) == NO_ERROR;
}

void SetSystemPagesAccessInternal(
    uintptr_t address,
    size_t length,
    PageAccessibilityConfiguration accessibility) {
  if (accessibility == PageAccessibilityConfiguration::kInaccessible) {
    APIRET arc = MyDosSetMem(address, length, PAG_DECOMMIT);
    if (arc != NO_ERROR) {
      // We check `arc` for `NO_ERROR` here so that in a crash
      // report we get the error number.
      CHECK_EQ(static_cast<ULONG>(NO_ERROR), arc);
    }
  } else {
    APIRET arc = MyDosSetMem(address, length, PAG_COMMIT |
                             GetAccessFlags(accessibility));
    if (arc != NO_ERROR) {
      if (arc == ERROR_NOT_ENOUGH_MEMORY)
        OOM_CRASH(length);
      // We check `arc` for `NO_ERROR` here so that in a crash
      // report we get the arc number.
      CHECK_EQ(static_cast<ULONG>(NO_ERROR), arc);
    }
  }
}

void FreePagesInternal(uintptr_t address, size_t length) {
  APIRET arc = DosFreeMemEx(address);
  CHECK_EQ(static_cast<ULONG>(NO_ERROR), arc);
}

void DecommitSystemPagesInternal(
    uintptr_t address,
    size_t length,
    PageAccessibilityDisposition accessibility_disposition) {
  // Ignore accessibility_disposition, because decommitting is equivalent to
  // making pages inaccessible.
  SetSystemPagesAccess(address, length,
                       PageAccessibilityConfiguration::kInaccessible);
}

void DecommitAndZeroSystemPagesInternal(uintptr_t address, size_t length) {
  PA_CHECK(MyDosSetMem(reinterpret_cast<void*>(address), length, PAG_DECOMMIT));
}

void RecommitSystemPagesInternal(
    uintptr_t address,
    size_t length,
    PageAccessibilityConfiguration accessibility,
    PageAccessibilityDisposition accessibility_disposition) {
  // Ignore accessibility_disposition, because decommitting is equivalent to
  // making pages inaccessible.
  SetSystemPagesAccess(address, length, accessibility);
}

bool TryRecommitSystemPagesInternal(
    uintptr_t address,
    size_t length,
    PageAccessibilityConfiguration accessibility,
    PageAccessibilityDisposition accessibility_disposition) {
  // Ignore accessibility_disposition, because decommitting is equivalent to
  // making pages inaccessible.
  return TrySetSystemPagesAccess(address, length, accessibility);
}

void DiscardSystemPagesInternal(uintptr_t address, size_t length) {
  // TODO: OS/2 doen't seem to have madvise(MADV_DONTNEED)/MEM_RESET semantics.
}

}  // namespace namespace partition_alloc::internal

#endif  // BASE_ALLOCATOR_PARTITION_ALLOCATOR_PAGE_ALLOCATOR_INTERNALS_OS2_H_
