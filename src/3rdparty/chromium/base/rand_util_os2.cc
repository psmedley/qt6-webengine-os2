// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/rand_util.h"

#include <stdlib.h>

#include <algorithm>

#include "base/containers/span.h"
#include "base/lazy_instance.h"

namespace {

// OS/2 doesn't have /dev/[u]random, use BSD random instead.
class BSDRandom {
 public:
  BSDRandom() { srandomdev(); }
};

base::LazyInstance<BSDRandom>::Leaky g_bsd_random = LAZY_INSTANCE_INITIALIZER;

}  // namespace

namespace base {

namespace internal {

void ConfigureBoringSSLBackedRandBytesFieldTrial() {
  // OS/2 always uses BSD random() above; there is no BoringSSL-backed
  // variant to configure.
}

double RandDoubleAvoidAllocation() {
  // RandBytes() below doesn't allocate (LazyInstance uses static storage and
  // random() doesn't allocate), so it's safe to call from the allocator shim.
  uint64_t number;
  RandBytes(&number, sizeof(number));
  // This transformation is explained in rand_util.cc.
  return (number >> 11) * 0x1.0p-53;
}

}  // namespace internal

void RandBytes(span<uint8_t> output) {
  // trigger a thread-safe one-time srandomdev() call.
  g_bsd_random.Pointer();
  uint8_t* output_ptr = output.data();
  size_t output_length = output.size();
  while (output_length > 0) {
    // random() returns a 31 bit random number, use the low 24 bits.
    size_t len = std::min(output_length, static_cast<size_t>(3));
    long r = random();
    output_length -= len;
    while (len--) {
      *output_ptr++ = static_cast<uint8_t>(r);
      r >>= 8;
    }
  }
}

void RandBytes(void* output, size_t output_length) {
  RandBytes(make_span(static_cast<uint8_t*>(output), output_length));
}

}  // namespace base
