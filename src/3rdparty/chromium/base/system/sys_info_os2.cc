// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/os2/os2_toolkit.h"

#include "base/system/sys_info.h"

#include "base/logging.h"

namespace base {

// static
uint64_t SysInfo::AmountOfPhysicalMemoryImpl() {
  ULONG data;
  if (DosQuerySysInfo(QSV_TOTPHYSMEM, QSV_TOTPHYSMEM, &data, sizeof(data)))
    return 0;
  return data;
}

// static
uint64_t SysInfo::AmountOfAvailablePhysicalMemoryImpl() {
  ULONG data;
  if (DosQuerySysInfo(QSV_TOTAVAILMEM, QSV_TOTAVAILMEM, &data, sizeof(data)))
    return 0;
  return data;
}

// static
SysInfo::HardwareInfo SysInfo::GetHardwareInfoSync() {
  // No portable way to query the machine manufacturer/model on OS/2; empty
  // strings mean "unknown" per sys_info.h.
  return HardwareInfo();
}

// static
std::string SysInfo::CPUModelName() {
  return "i386";
}

}  // namespace base
