// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/allocator/partition_allocator/partition_alloc_base/rand_util.h"

#include <stddef.h>
#include <stdlib.h>

#include <algorithm>

namespace {

// OS/2 doesn't have /dev/[u]random, use BSD random instead.
// Mirrors base/rand_util_os2.cc.
void EnsureSeeded() {
  // Function-local static initialization is thread-safe and runs once.
  static const bool seeded = [] {
    srandomdev();
    return true;
  }();
  (void)seeded;
}

}  // namespace

namespace partition_alloc::internal::base {

void RandBytes(void* output, size_t output_length) {
  EnsureSeeded();
  char* output_ptr = static_cast<char*>(output);
  while (output_length > 0) {
    // random() returns a 31 bit random number, use the low 24 bits.
    size_t len = std::min(output_length, static_cast<size_t>(3));
    long r = random();
    output_length -= len;
    while (len--) {
      *output_ptr++ = static_cast<char>(r);
      r >>= 8;
    }
  }
}

}  // namespace partition_alloc::internal::base
