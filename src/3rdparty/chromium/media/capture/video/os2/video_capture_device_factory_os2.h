// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_CAPTURE_VIDEO_FUCHSIA_VIDEO_CAPTURE_DEVICE_FACTORY_OS2_H_
#define MEDIA_CAPTURE_VIDEO_FUCHSIA_VIDEO_CAPTURE_DEVICE_FACTORY_OS2_H_

#include <map>

#include "base/containers/small_map.h"
#include "media/capture/video/video_capture_device_factory.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace media {

class CAPTURE_EXPORT VideoCaptureDeviceFactoryOS2
    : public VideoCaptureDeviceFactory {
 public:
  VideoCaptureDeviceFactoryOS2();
  ~VideoCaptureDeviceFactoryOS2() override;

  // VideoCaptureDeviceFactory implementation.
  VideoCaptureErrorOrDevice CreateDevice(
      const VideoCaptureDeviceDescriptor& device_descriptor) override;
  void GetDevicesInfo(GetDevicesInfoCallback callback);
  void GetDeviceDescriptors(
      VideoCaptureDeviceDescriptors* device_descriptors);
  void GetSupportedFormats(const VideoCaptureDeviceDescriptor& device,
                           VideoCaptureFormats* supported_formats);
};

}  // namespace media

#endif  // MEDIA_CAPTURE_VIDEO_FUCHSIA_VIDEO_CAPTURE_DEVICE_FACTORY_OS2_H_
