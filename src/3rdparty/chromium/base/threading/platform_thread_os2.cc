// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/threading/platform_thread.h"

#include "base/threading/platform_thread_internal_posix.h"
#include "base/threading/thread_id_name_manager.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

#include "base/os2/os2_toolkit.h"

namespace base {

namespace internal {

namespace {

// OS/2 priority classes, from lowest to highest: PRTYC_IDLETIME,
// PRTYC_REGULAR (where the desktop and normal applications run),
// PRTYC_FOREGROUNDSERVER and PRTYC_TIMECRITICAL. Threads in a higher class
// always run before threads in a lower one, so only threads that do very
// little work per wakeup may be placed above PRTYC_REGULAR; otherwise a busy
// page (e.g. video playback) starves the whole desktop. Within a class the
// level ranges from 0 (the default) to PRTYD_MAXIMUM (31).
//
// - BACKGROUND:      PRTYC_IDLETIME
// - NORMAL:          PRTYC_REGULAR, level 0 (the default for new threads)
// - DISPLAY:         PRTYC_REGULAR, raised level. Used by the renderer main
//                    and compositor threads, the browser UI/IO threads, Viz
//                    and GPU threads, which can be CPU-bound for long periods.
// - REALTIME_AUDIO:  PRTYC_FOREGROUNDSERVER, level 0. Audio threads only do a
//                    short amount of work per buffer, but must not be
//                    starved by regular threads.
constexpr LONG kDisplayLevel = 16;

struct OS2Priority {
  ULONG cls;
  LONG level;
};

OS2Priority ToOS2Priority(ThreadPriority priority) {
  switch (priority) {
    case ThreadPriority::BACKGROUND:
      return {PRTYC_IDLETIME, 0};
    case ThreadPriority::NORMAL:
      return {PRTYC_REGULAR, 0};
    case ThreadPriority::DISPLAY:
      return {PRTYC_REGULAR, kDisplayLevel};
    case ThreadPriority::REALTIME_AUDIO:
      return {PRTYC_FOREGROUNDSERVER, 0};
  }
  return {PRTYC_REGULAR, 0};
}

// Returns the current thread's priority class and level.
bool GetCurrentOS2Priority(ULONG* cls, LONG* level) {
  PTIB ptib;
  if (DosGetInfoBlocks(&ptib, NULL) != NO_ERROR)
    return false;
  // The priority class is in the high byte of the low word, the level in the
  // low byte.
  ULONG pri = ptib->tib_ptib2->tib2_ulpri;
  *cls = (pri >> 8) & 0xFF;
  *level = pri & 0xFF;
  return true;
}

}  // namespace

bool SetCurrentThreadPriorityForPlatform(ThreadPriority priority) {
  const OS2Priority target = ToOS2Priority(priority);
  // Changing the class resets the level, so set the class first and then
  // move the level to the target (|delta| is relative to the current level).
  // A thread ID of 0 means the current thread.
  if (DosSetPriority(PRTYS_THREAD, target.cls, 0, 0) != NO_ERROR)
    return false;
  ULONG cls;
  LONG level;
  if (!GetCurrentOS2Priority(&cls, &level))
    return false;
  if (level != target.level &&
      DosSetPriority(PRTYS_THREAD, PRTYC_NOCHANGE, target.level - level, 0) !=
          NO_ERROR) {
    return false;
  }
  return true;
}

absl::optional<ThreadPriority> GetCurrentThreadPriorityForPlatform() {
  ULONG cls;
  LONG level;
  if (!GetCurrentOS2Priority(&cls, &level))
    return absl::nullopt;
  switch (cls) {
    case PRTYC_IDLETIME:
      return ThreadPriority::BACKGROUND;
    case PRTYC_REGULAR:
      return level >= kDisplayLevel ? ThreadPriority::DISPLAY
                                    : ThreadPriority::NORMAL;
    case PRTYC_FOREGROUNDSERVER:
    case PRTYC_TIMECRITICAL:
      return ThreadPriority::REALTIME_AUDIO;
  }
  return absl::nullopt;
}

}  // namespace internal

void InitThreading() {}

void TerminateOnThread() {}

size_t GetDefaultThreadStackSize(const pthread_attr_t& attributes) {
  return 0;
}

// static
void PlatformThread::SetName(const std::string& name) {
  ThreadIdNameManager::GetInstance()->SetName(name);
}

}  // namespace base
