// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "partition_alloc/oom.h"

#include "build/build_config.h"
#include "partition_alloc/oom_callback.h"
#include "partition_alloc/partition_alloc_base/compiler_specific.h"
#include "partition_alloc/partition_alloc_base/debug/alias.h"
#include "partition_alloc/partition_alloc_base/immediate_crash.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include <array>
#include <cstdlib>
#endif  // BUILDFLAG(IS_WIN)

#if BUILDFLAG(IS_OS2)
#define INCL_DOSMISC
#include <os2.h>
#include <umalloc.h>

#include "partition_alloc/partition_alloc_base/logging.h"
#endif  // BUILDFLAG(IS_OS2)

namespace partition_alloc {

size_t g_oom_size = 0U;

namespace internal {

// Crash server classifies base::internal::OnNoMemoryInternal as OOM.
// TODO(crbug.com/1151236): Update to
// partition_alloc::internal::base::internal::OnNoMemoryInternal
PA_NOINLINE void OnNoMemoryInternal(size_t size) {
  g_oom_size = size;
#if BUILDFLAG(IS_WIN)
  // Kill the process. This is important for security since most of code
  // does not check the result of memory allocation.
  // https://msdn.microsoft.com/en-us/library/het71c37.aspx
  // Pass the size of the failed request in an exception argument.
  ULONG_PTR exception_args[] = {size};
  ::RaiseException(win::kOomExceptionCode, EXCEPTION_NONCONTINUABLE,
                   std::size(exception_args), exception_args);

  // Safety check, make sure process exits here.
  _exit(win::kOomExceptionCode);
#else
  size_t tmp_size = size;
  internal::base::debug::Alias(&tmp_size);

#if BUILDFLAG(IS_OS2)
  PA_LOG(ERROR) << "Out of memory. size=" << tmp_size;
  // Log some numbers from LIBC and system before crashing.
  _HEAPSTATS hst;
  if (!_ustats(_udefault(nullptr), &hst))
    PA_LOG(ERROR) << "LIBC heap: provided " << hst._provided << ", used "
                  << hst._used << ", maxfree " << hst._max_free;
  else
    PA_PLOG(ERROR) << "_ustats failed";
  ULONG buf[2];
  if (!DosQuerySysInfo(QSV_MAXPRMEM, QSV_MAXSHMEM, buf, sizeof(buf)))
    PA_LOG(ERROR) << "System low mem: max private " << buf[0]
                  << ", max shared " << buf[1];
  if (!DosQuerySysInfo(QSV_MAXHPRMEM, QSV_MAXHSHMEM, buf, sizeof(buf)))
    PA_LOG(ERROR) << "System high mem: max private " << buf[0]
                  << ", max shared " << buf[1];
#endif  // BUILDFLAG(IS_OS2)

  // Note: Don't add anything that may allocate here. Depending on the
  // allocator, this may be called from within the allocator (e.g. with
  // PartitionAlloc), and would deadlock as our locks are not recursive.
  //
  // Additionally, this is unlikely to work, since allocating from an OOM
  // handler is likely to fail.
  //
  // Use PA_IMMEDIATE_CRASH() so that the top frame in the crash is our code,
  // rather than using abort() or similar; this avoids the crash server needing
  // to be able to successfully unwind through libc to get to the correct
  // address, which is particularly an issue on Android.
  PA_IMMEDIATE_CRASH();
#endif  // BUILDFLAG(IS_WIN)
}

}  // namespace internal

void TerminateBecauseOutOfMemory(size_t size) {
  internal::OnNoMemoryInternal(size);
}

namespace internal {

// The crash is generated in a PA_NOINLINE function so that we can classify the
// crash as an OOM solely by analyzing the stack trace. It is tagged as
// PA_NOT_TAIL_CALLED to ensure that its parent function stays on the stack.
[[noreturn]] PA_NOINLINE PA_NOT_TAIL_CALLED void OnNoMemory(size_t size) {
  RunPartitionAllocOomCallback();
  TerminateBecauseOutOfMemory(size);
  PA_IMMEDIATE_CRASH();
}

}  // namespace internal

}  // namespace partition_alloc
