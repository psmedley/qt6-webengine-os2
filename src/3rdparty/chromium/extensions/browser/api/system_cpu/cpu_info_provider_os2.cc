// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/browser/api/system_cpu/cpu_info_provider.h"

#include <stdint.h>

#include <cstdio>
#include <sstream>

#include "base/files/file_util.h"
#include "base/format_macros.h"

namespace extensions {


bool CpuInfoProvider::QueryCpuTimePerProcessor(
    std::vector<api::system_cpu::ProcessorInfo>* infos) {
  DCHECK(infos);
  // stub only for now
  return true;
}

}  // namespace extensions
