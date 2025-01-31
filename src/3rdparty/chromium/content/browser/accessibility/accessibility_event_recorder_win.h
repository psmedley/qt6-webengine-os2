// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_ACCESSIBILITY_ACCESSIBILITY_EVENT_RECORDER_WIN_H_
#define CONTENT_BROWSER_ACCESSIBILITY_ACCESSIBILITY_EVENT_RECORDER_WIN_H_

#include <oleacc.h>

#include "base/win/windows_types.h"
#include "content/browser/accessibility/accessibility_event_recorder.h"
#include "content/common/content_export.h"

namespace content {

class CONTENT_EXPORT AccessibilityEventRecorderWin
    : public AccessibilityEventRecorder {
 public:
  AccessibilityEventRecorderWin(BrowserAccessibilityManager* manager,
                                base::ProcessId pid,
                                const ui::AXTreeSelector& selector);
  ~AccessibilityEventRecorderWin() override;

  // Callback registered by SetWinEventHook. Just calls OnWinEventHook.
  static void CALLBACK WinEventHookThunk(HWINEVENTHOOK handle,
                                         DWORD event,
                                         HWND hwnd,
                                         LONG obj_id,
                                         LONG child_id,
                                         DWORD event_thread,
                                         DWORD event_time);

 private:
  // Called by the thunk registered by SetWinEventHook. Retrieves accessibility
  // info about the node the event was fired on and appends a string to
  // the event log.
  void OnWinEventHook(HWINEVENTHOOK handle,
                      DWORD event,
                      HWND hwnd,
                      LONG obj_id,
                      LONG child_id,
                      DWORD event_thread,
                      DWORD event_time);

  // Wrapper around AccessibleObjectFromWindow because the function call
  // inexplicably flakes sometimes on build/trybots.
  HRESULT AccessibleObjectFromWindowWrapper(HWND hwnd,
                                            DWORD dwId,
                                            REFIID riid,
                                            void** ppvObject);

  HWINEVENTHOOK win_event_hook_handle_;
  static AccessibilityEventRecorderWin* instance_;

  DISALLOW_COPY_AND_ASSIGN(AccessibilityEventRecorderWin);
};

}  // namespace content

#endif  // CONTENT_BROWSER_ACCESSIBILITY_ACCESSIBILITY_EVENT_RECORDER_WIN_H_
