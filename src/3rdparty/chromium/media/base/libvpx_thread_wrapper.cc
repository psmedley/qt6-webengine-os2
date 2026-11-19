// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/libvpx_thread_wrapper.h"

#include "build/build_config.h"

#if !BUILDFLAG(IS_OS2)
#include "media/base/codec_worker_impl.h"
#include "third_party/libvpx/source/libvpx/vpx_util/vpx_thread.h"
#endif

namespace media {

void InitLibVpxThreadWrapper() {
#if BUILDFLAG(IS_OS2)
  // OS/2 uses the system libvpx, which has neither the internal
  // vpx_util/vpx_thread.h header nor Chromium's vpx_set_worker_interface()
  // hook; libvpx keeps using its own threads.
#else
  const VPxWorkerInterface interface =
      CodecWorkerImpl<VPxWorkerInterface, VPxWorkerImpl, VPxWorker,
                      VPxWorkerStatus, NOT_OK, OK,
                      WORK>::GetCodecWorkerInterface();

  CHECK(vpx_set_worker_interface(&interface));
#endif
}

}  // namespace media
