// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// OS/2 implementation. There is no portable way to query the active PM
// keyboard layout here, so this follows a US keyboard layout, like the
// Ozone stub keyboard layout engine does.

#include "chrome/test/chromedriver/keycode_text_conversion.h"

#include <string>

#include "base/strings/utf_string_conversion_utils.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/test/chromedriver/chrome/ui_events.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"

bool ConvertKeyCodeToText(ui::KeyboardCode key_code,
                          int modifiers,
                          std::string* text,
                          std::string* error_msg) {
  *error_msg = std::string();
  ui::DomCode dom_code = ui::UsLayoutKeyboardCodeToDomCode(key_code);
  int event_flags = ui::EF_NONE;
  if (modifiers & kAltKeyModifierMask)
    event_flags |= ui::EF_ALT_DOWN;
  if (modifiers & kControlKeyModifierMask)
    event_flags |= ui::EF_CONTROL_DOWN;
  if (modifiers & kShiftKeyModifierMask)
    event_flags |= ui::EF_SHIFT_DOWN;

  ui::DomKey dom_key;
  ui::KeyboardCode key_code_ignored;
  *text = std::string();
  if (!ui::DomCodeToUsLayoutDomKey(dom_code, event_flags, &dom_key,
                                   &key_code_ignored) ||
      !dom_key.IsCharacter()) {
    return true;
  }
  base_icu::UChar32 c = dom_key.ToCharacter();
  // Filter out control characters (e.g. Ctrl+letter, Tab, Enter).
  if (c < 0x20 || c == 0x7f)
    return true;
  base::WriteUnicodeCharacter(c, text);
  return true;
}

bool ConvertCharToKeyCode(char16_t key,
                          ui::KeyboardCode* key_code,
                          int* necessary_modifiers,
                          std::string* error_msg) {
  std::string key_string_utf8 = base::UTF16ToUTF8(std::u16string(1, key));
  *error_msg = std::string();
  for (int i = 0; i < 256; ++i) {
    ui::KeyboardCode code = static_cast<ui::KeyboardCode>(i);
    // Skip the numpad keys.
    if (code >= ui::VKEY_NUMPAD0 && code <= ui::VKEY_DIVIDE)
      continue;
    std::string key_string;
    if (!ConvertKeyCodeToText(code, 0, &key_string, error_msg))
      return false;
    if (key_string_utf8 == key_string) {
      *key_code = code;
      *necessary_modifiers = 0;
      return true;
    }
    if (!ConvertKeyCodeToText(code, kShiftKeyModifierMask, &key_string,
                              error_msg))
      return false;
    if (key_string_utf8 == key_string) {
      *key_code = code;
      *necessary_modifiers = kShiftKeyModifierMask;
      return true;
    }
  }
  return false;
}
