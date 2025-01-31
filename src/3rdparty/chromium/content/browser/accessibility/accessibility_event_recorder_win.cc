// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/accessibility/accessibility_event_recorder_win.h"

#include <stdint.h>
#include <wrl/client.h>

#include <string>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_piece.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/win/scoped_bstr.h"
#include "base/win/scoped_variant.h"
#include "content/browser/accessibility/accessibility_event_recorder_uia_win.h"
#include "content/browser/accessibility/browser_accessibility_manager.h"
#include "content/browser/accessibility/browser_accessibility_win.h"
#include "third_party/iaccessible2/ia2_api_all.h"
#include "ui/accessibility/platform/inspect/ax_inspect_utils_win.h"
#include "ui/base/win/atl_module.h"
#include "ui/gfx/win/hwnd_util.h"

namespace content {

using ui::AccessibilityEventToString;
using ui::GetHWNDBySelector;
using ui::IAccessible2StateToString;
using ui::IAccessibleRoleToString;
using ui::IAccessibleStateToString;

namespace {

std::string RoleVariantToString(const base::win::ScopedVariant& role) {
  if (role.type() == VT_I4) {
    return base::WideToUTF8(IAccessibleRoleToString(V_I4(role.ptr())));
  } else if (role.type() == VT_BSTR) {
    return base::WideToUTF8(
        std::wstring(V_BSTR(role.ptr()), SysStringLen(V_BSTR(role.ptr()))));
  }
  return std::string();
}

HRESULT QueryIAccessible2(IAccessible* accessible, IAccessible2** accessible2) {
  Microsoft::WRL::ComPtr<IServiceProvider> service_provider;
  HRESULT hr = accessible->QueryInterface(IID_PPV_ARGS(&service_provider));
  return SUCCEEDED(hr)
             ? service_provider->QueryService(IID_IAccessible2, accessible2)
             : hr;
}

HRESULT QueryIAccessibleText(IAccessible* accessible,
                             IAccessibleText** accessible_text) {
  Microsoft::WRL::ComPtr<IServiceProvider> service_provider;
  HRESULT hr = accessible->QueryInterface(IID_PPV_ARGS(&service_provider));
  return SUCCEEDED(hr) ? service_provider->QueryService(IID_IAccessibleText,
                                                        accessible_text)
                       : hr;
}

std::string BstrToPrettyUTF8(BSTR bstr) {
  std::wstring wstr(bstr, SysStringLen(bstr));

  // IAccessibleText returns the text you get by appending all static text
  // children, with an "embedded object character" for each non-text child.
  // Pretty-print the embedded object character as <obj> so that test output
  // is human-readable.
#ifndef TOOLKIT_QT
  std::wstring embedded_character = base::UTF16ToWide(
      std::u16string(1, BrowserAccessibilityComWin::kEmbeddedCharacter));
  base::ReplaceChars(wstr, embedded_character, L"<obj>", &wstr);
#endif

  return base::WideToUTF8(wstr);
}

std::string AccessibilityEventToStringUTF8(int32_t event_id) {
  return base::WideToUTF8(AccessibilityEventToString(event_id));
}

}  // namespace

// static
AccessibilityEventRecorderWin* AccessibilityEventRecorderWin::instance_ =
    nullptr;

// static
void CALLBACK AccessibilityEventRecorderWin::WinEventHookThunk(
    HWINEVENTHOOK handle,
    DWORD event,
    HWND hwnd,
    LONG obj_id,
    LONG child_id,
    DWORD event_thread,
    DWORD event_time) {
  if (instance_) {
    instance_->OnWinEventHook(handle, event, hwnd, obj_id, child_id,
                              event_thread, event_time);
  }
}

AccessibilityEventRecorderWin::AccessibilityEventRecorderWin(
    BrowserAccessibilityManager* manager,
    base::ProcessId pid,
    const ui::AXTreeSelector& selector)
    : AccessibilityEventRecorder(manager) {
  CHECK(!instance_) << "There can be only one instance of"
                    << " AccessibilityEventRecorder at a time.";

  // Get pid by a selector if the selectors specifies a valid process.
  HWND hwnd_by_selector = GetHWNDBySelector(selector);
  if (hwnd_by_selector != NULL) {
    DWORD pid_by_selector = 0;
    GetWindowThreadProcessId(hwnd_by_selector, &pid_by_selector);
    if (pid_by_selector != 0) {
      pid = static_cast<DWORD>(pid_by_selector);
    }
  }

  // For now, just use out of context events when running as a utility to watch
  // events (no BrowserAccessibilityManager), because otherwise Chrome events
  // are not getting reported. Being in context is better so that for
  // TEXT_REMOVED and TEXT_INSERTED events, we can query the text that was
  // inserted or removed and include that in the log.
  int context = manager ? WINEVENT_INCONTEXT : WINEVENT_OUTOFCONTEXT;
  win_event_hook_handle_ =
      SetWinEventHook(EVENT_MIN, EVENT_MAX, GetModuleHandle(NULL),
                      &AccessibilityEventRecorderWin::WinEventHookThunk, pid,
                      0,  // Hook all threads
                      context);
  CHECK(win_event_hook_handle_);
  instance_ = this;
}

AccessibilityEventRecorderWin::~AccessibilityEventRecorderWin() {
  UnhookWinEvent(win_event_hook_handle_);
  instance_ = nullptr;
}

void AccessibilityEventRecorderWin::OnWinEventHook(HWINEVENTHOOK handle,
                                                   DWORD event,
                                                   HWND hwnd,
                                                   LONG obj_id,
                                                   LONG child_id,
                                                   DWORD event_thread,
                                                   DWORD event_time) {
  Microsoft::WRL::ComPtr<IAccessible> browser_accessible;
  HRESULT hr = AccessibleObjectFromWindowWrapper(
      hwnd, obj_id, IID_PPV_ARGS(&browser_accessible));
  if (FAILED(hr)) {
    // Note: our event hook will pick up some superfluous events we
    // don't care about, so it's safe to just ignore these failures.
    // Same below for other HRESULT checks.
    VLOG(1) << "Ignoring result " << hr << " from AccessibleObjectFromWindow";
    return;
  }

  base::win::ScopedVariant childid_variant(child_id);
  Microsoft::WRL::ComPtr<IDispatch> dispatch;
  hr = browser_accessible->get_accChild(childid_variant, &dispatch);
  if (hr != S_OK || !dispatch) {
    VLOG(1) << "Ignoring result " << hr << " and result " << dispatch.Get()
            << " from get_accChild";
    return;
  }

  Microsoft::WRL::ComPtr<IAccessible> iaccessible;
  hr = dispatch.As(&iaccessible);
  if (FAILED(hr)) {
    VLOG(1) << "Ignoring result " << hr << " from QueryInterface";
    return;
  }

  std::string event_str = AccessibilityEventToStringUTF8(event);
  if (event_str.empty()) {
    VLOG(1) << "Ignoring event " << event;
    return;
  }

  base::win::ScopedVariant childid_self(CHILDID_SELF);
  base::win::ScopedVariant role;
  iaccessible->get_accRole(childid_self, role.Receive());
  base::win::ScopedVariant state;
  iaccessible->get_accState(childid_self, state.Receive());
  int ia_state = V_I4(state.ptr());
  std::string hwnd_class_name = base::WideToUTF8(gfx::GetClassName(hwnd));

  // Caret is special:
  // Log all caret events  that occur, with their window class, so that we can
  // test to make sure they are only occurring on the desired window class.
  if (ROLE_SYSTEM_CARET == V_I4(role.ptr())) {
    std::wstring state_str = IAccessibleStateToString(ia_state);
    std::string log = base::StringPrintf(
        "%s role=ROLE_SYSTEM_CARET %ls window_class=%s", event_str.c_str(),
        state_str.c_str(), hwnd_class_name.c_str());
    OnEvent(log);
    return;
  }

  if (only_web_events_) {
    if (hwnd_class_name != "Chrome_RenderWidgetHostHWND")
      return;

    Microsoft::WRL::ComPtr<IServiceProvider> service_provider;
    hr = iaccessible->QueryInterface(IID_PPV_ARGS(&service_provider));
    if (FAILED(hr))
      return;

    Microsoft::WRL::ComPtr<IAccessible> content_document;
    hr = service_provider->QueryService(GUID_IAccessibleContentDocument,
                                        IID_PPV_ARGS(&content_document));
    if (FAILED(hr))
      return;
  }

  base::win::ScopedBstr name_bstr;
  iaccessible->get_accName(childid_self, name_bstr.Receive());
  base::win::ScopedBstr value_bstr;
  iaccessible->get_accValue(childid_self, value_bstr.Receive());

  // Avoid flakiness. Events fired on a WINDOW are out of the control
  // of a test.
  if (role.type() == VT_I4 && ROLE_SYSTEM_WINDOW == V_I4(role.ptr())) {
    VLOG(1) << "Ignoring event " << event << " on ROLE_SYSTEM_WINDOW";
    return;
  }

  // Avoid flakiness. The "offscreen" state depends on whether the browser
  // window is frontmost or not, and "hottracked" depends on whether the
  // mouse cursor happens to be over the element.
  ia_state &= (~STATE_SYSTEM_OFFSCREEN & ~STATE_SYSTEM_HOTTRACKED);

  // The "readonly" state is set on almost every node and doesn't typically
  // change, so filter it out to keep the output less verbose.
  ia_state &= ~STATE_SYSTEM_READONLY;

  AccessibleStates ia2_state = 0;
  Microsoft::WRL::ComPtr<IAccessible2> iaccessible2;
  hr = QueryIAccessible2(iaccessible.Get(), &iaccessible2);
  bool has_ia2 = SUCCEEDED(hr) && iaccessible2;

  std::wstring html_tag;
  std::wstring obj_class;
  std::wstring html_id;

  if (has_ia2) {
    iaccessible2->get_states(&ia2_state);
    base::win::ScopedBstr attributes_bstr;
    if (S_OK == iaccessible2->get_attributes(attributes_bstr.Receive())) {
      std::vector<std::wstring> ia2_attributes = base::SplitString(
          std::wstring(attributes_bstr.Get(), attributes_bstr.Length()),
          std::wstring(1, ';'), base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
      for (std::wstring& attr : ia2_attributes) {
        if (base::StartsWith(attr, L"class:"))
          obj_class = attr.substr(6);  // HTML or view class
        if (base::StartsWith(attr, L"id:")) {
          html_id = std::wstring(L"#");
          html_id += attr.substr(3);
        }
        if (base::StartsWith(attr, L"tag:")) {
          html_tag = attr.substr(4);
        }
      }
    }
  }

  std::string log = base::StringPrintf("%s on", event_str.c_str());
  if (!html_tag.empty()) {
    // HTML node with tag
    log += base::StringPrintf(" <%s%s%s%s>", base::WideToUTF8(html_tag).c_str(),
                              base::WideToUTF8(html_id).c_str(),
                              obj_class.empty() ? "" : ".",
                              base::WideToUTF8(obj_class).c_str());
  } else if (!obj_class.empty()) {
    // Non-HTML node with class
    log += base::StringPrintf(" class=%s", base::WideToUTF8(obj_class).c_str());
  }

  log += base::StringPrintf(" role=%s", RoleVariantToString(role).c_str());
  if (name_bstr.Length() > 0)
    log += base::StringPrintf(" name=\"%s\"",
                              BstrToPrettyUTF8(name_bstr.Get()).c_str());
  if (value_bstr.Length() > 0) {
    bool is_document =
        role.type() == VT_I4 && ROLE_SYSTEM_DOCUMENT == V_I4(role.ptr());
    // Don't show actual document value, which is a URL, in order to avoid
    // machine-based differences in tests.
    log += is_document
               ? " value~=[doc-url]"
               : base::StringPrintf(" value=\"%s\"",
                                    BstrToPrettyUTF8(value_bstr.Get()).c_str());
  }
  log += " ";
  log += base::WideToUTF8(IAccessibleStateToString(ia_state));
  log += " ";
  log += base::WideToUTF8(IAccessible2StateToString(ia2_state));

  // Group position, e.g. L3, 5 of 7
  LONG group_level, similar_items_in_group, position_in_group;
  if (has_ia2 &&
      iaccessible2->get_groupPosition(&group_level, &similar_items_in_group,
                                      &position_in_group) == S_OK) {
    if (group_level)
      log += base::StringPrintf(" level=%ld", group_level);
    if (position_in_group)
      log += base::StringPrintf(" PosInSet=%ld", position_in_group);
    if (similar_items_in_group)
      log += base::StringPrintf(" SetSize=%ld", similar_items_in_group);
  }

  // For TEXT_REMOVED and TEXT_INSERTED events, query the text that was
  // inserted or removed and include that in the log.
  Microsoft::WRL::ComPtr<IAccessibleText> accessible_text;
  hr = QueryIAccessibleText(iaccessible.Get(), &accessible_text);
  if (SUCCEEDED(hr)) {
    if (event == IA2_EVENT_TEXT_REMOVED) {
      IA2TextSegment old_text;
      if (SUCCEEDED(accessible_text->get_oldText(&old_text))) {
        log += base::StringPrintf(" old_text={'%s' start=%ld end=%ld}",
                                  BstrToPrettyUTF8(old_text.text).c_str(),
                                  old_text.start, old_text.end);
      }
    }
    if (event == IA2_EVENT_TEXT_INSERTED) {
      IA2TextSegment new_text;
      if (SUCCEEDED(accessible_text->get_newText(&new_text))) {
        log += base::StringPrintf(" new_text={'%s' start=%ld end=%ld}",
                                  BstrToPrettyUTF8(new_text.text).c_str(),
                                  new_text.start, new_text.end);
      }
    }
  }

  log =
      base::UTF16ToUTF8(base::CollapseWhitespace(base::UTF8ToUTF16(log), true));
  OnEvent(log);
}

HRESULT AccessibilityEventRecorderWin::AccessibleObjectFromWindowWrapper(
    HWND hwnd,
    DWORD dw_id,
    REFIID riid,
    void** ppv_object) {
#ifndef TOOLKIT_QT
  HRESULT hr = ::AccessibleObjectFromWindow(hwnd, dw_id, riid, ppv_object);
  if (SUCCEEDED(hr))
    return hr;

  // There used to be a use after free error here, because manager_ is a raw
  // pointer but the manager's owner RenderFrameHostImpl would release it when
  // RenderFrameHostImpl::AccessibilityFatalError() tried to gracefully reset
  // accessibility. However, it is no longer possible to reach here after an
  // AccessibilityFatalError(), because that code will force a crash when
  // developer features such as AccessibleEventRecorder is used.

  // The above call to ::AccessibleObjectFromWindow fails for unknown
  // reasons every once in a while on the bots.  Work around it by grabbing
  // the object directly from the BrowserAccessibilityManager.
  if (!manager_)  // No manager when outside of Chrome tests.
    return E_FAIL;

  HWND accessibility_hwnd =
      manager_->delegate()->AccessibilityGetAcceleratedWidget();
  if (accessibility_hwnd != hwnd)
    return E_FAIL;

  IAccessible* obj = ToBrowserAccessibilityComWin(manager_->GetRoot());
  obj->AddRef();
  *ppv_object = obj;
  return S_OK;
#else
  return E_FAIL;
#endif
}

}  // namespace content
