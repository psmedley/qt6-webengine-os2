// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdint>
#include <string>

#include "base/check_op.h"
#include "base/containers/flat_map.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/dom/dom_keyboard_layout_map_base.h"
#include "ui/events/ozone/layout/keyboard_layout_engine.h"
#include "ui/events/ozone/layout/keyboard_layout_engine_manager.h"

namespace ui {

namespace {

class DomKeyboardLayoutMapOS2 : public DomKeyboardLayoutMapBase {
 public:
  DomKeyboardLayoutMapOS2();
  ~DomKeyboardLayoutMapOS2() override;

 private:
  // ui::DomKeyboardLayoutMapBase implementation.
  uint32_t GetKeyboardLayoutCount() override;
  ui::DomKey GetDomKeyFromDomCodeForLayout(
      ui::DomCode dom_code,
      uint32_t keyboard_layout_index) override;

//  DISALLOW_COPY_AND_ASSIGN(DomKeyboardLayoutMapOS2);
};

DomKeyboardLayoutMapOS2::DomKeyboardLayoutMapOS2() = default;

DomKeyboardLayoutMapOS2::~DomKeyboardLayoutMapOS2() = default;

uint32_t DomKeyboardLayoutMapOS2::GetKeyboardLayoutCount() {
  // There is only one keyboard layout available on OS/2.
  return 1;
}

ui::DomKey DomKeyboardLayoutMapOS2::GetDomKeyFromDomCodeForLayout(
    ui::DomCode dom_code,
    uint32_t keyboard_layout_index) {
  DCHECK_NE(dom_code, ui::DomCode::NONE);
  DCHECK_EQ(keyboard_layout_index, 0U);

#if 0
  ui::KeyboardLayoutEngine* const keyboard_layout_engine =
      ui::KeyboardLayoutEngineManager::GetKeyboardLayoutEngine();

  ui::DomKey dom_key;
  ui::KeyboardCode unused_code;
  if (keyboard_layout_engine->Lookup(dom_code, 0, &dom_key, &unused_code))
    return dom_key;
#endif
  return ui::DomKey::NONE;
}

}  // namespace

// static
base::flat_map<std::string, std::string> GenerateDomKeyboardLayoutMap() {
  return DomKeyboardLayoutMapOS2().Generate();
}

}  // namespace ui
