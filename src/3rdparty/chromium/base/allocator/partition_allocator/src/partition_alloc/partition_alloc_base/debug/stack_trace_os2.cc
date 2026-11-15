// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// OS/2 has neither /proc/self/maps nor <link.h>/ELF, which
// stack_trace_posix.cc relies on. As in //base, in-process stack dumping is
// not supported on OS/2: LIBC/EXCEPTQ produce a .TRP file with a stack dump
// instead. Provide the minimal API PartitionAlloc expects.

#include "partition_alloc/partition_alloc_base/debug/stack_trace.h"

#include "build/build_config.h"
#include "partition_alloc/partition_alloc_base/debug/debugging_buildflags.h"
#include "partition_alloc/partition_alloc_base/logging.h"
#include "partition_alloc/partition_alloc_base/strings/safe_sprintf.h"

namespace partition_alloc::internal::base::debug {

size_t CollectStackTrace(const void** trace, size_t count) {
  // NOTE: This code MUST be async-signal safe. NO malloc or stdio is allowed.
#if BUILDFLAG(PA_CAN_UNWIND_WITH_FRAME_POINTERS)
  return base::debug::TraceStackFramePointers(trace, count, 0);
#else
  // Not able to obtain stack traces.
  return 0;
#endif
}

void PrintStackTrace(const void** trace, size_t count) {
  // No module information is available; just print the raw addresses.
  for (size_t i = 0; i < count; ++i) {
    OutputStackTrace(static_cast<unsigned>(i),
                     reinterpret_cast<uintptr_t>(trace[i]), 0u, "???", 0u);
  }
}

void OutputStackTrace(unsigned index,
                      uintptr_t address,
                      uintptr_t base_address,
                      const char* module_name,
                      uintptr_t) {
  char buffer[256];
  strings::SafeSPrintf(buffer, "#%02d 0x%0x  (%s+0x%0x)\n", index, address,
                       module_name, address - base_address);
  PA_RAW_LOG(INFO, buffer);
}

}  // namespace partition_alloc::internal::base::debug
