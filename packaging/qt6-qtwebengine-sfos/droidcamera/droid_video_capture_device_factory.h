// RooTitanium — video capture factory via droidmedia (SailfishOS).
// Copyright (C) 2026 RootGPT. SPDX-License-Identifier: GPL-3.0-or-later
//
// It replaces the Linux V4L2 factory in
// CreatePlatformSpecificVideoCaptureDeviceFactory() ONLY when droidmedia really
// answers (symbols present and at least one camera). Otherwise everything stays
// as before: the same binary on a port without an Android HAL.

#ifndef MEDIA_CAPTURE_VIDEO_DROIDMEDIA_DROID_VIDEO_CAPTURE_DEVICE_FACTORY_H_
#define MEDIA_CAPTURE_VIDEO_DROIDMEDIA_DROID_VIDEO_CAPTURE_DEVICE_FACTORY_H_

#include "media/capture/video/video_capture_device_factory.h"

namespace media {

class DroidVideoCaptureDeviceFactory : public VideoCaptureDeviceFactory {
 public:
  // Computed once per process. It uses plain dlopen()/dlsym only, like
  // DroidVideoDecoder::PlatformSupported(): the droidmedia shim ABORTS when
  // asked for a symbol the device library does not have. So symbols are checked
  // BEFORE calling any droidmedia function.
  static bool IsSupported();

  DroidVideoCaptureDeviceFactory();
  DroidVideoCaptureDeviceFactory(const DroidVideoCaptureDeviceFactory&) =
      delete;
  DroidVideoCaptureDeviceFactory& operator=(
      const DroidVideoCaptureDeviceFactory&) = delete;
  ~DroidVideoCaptureDeviceFactory() override;

  VideoCaptureErrorOrDevice CreateDevice(
      const VideoCaptureDeviceDescriptor& device_descriptor) override;
  void GetDevicesInfo(GetDevicesInfoCallback callback) override;
};

}  // namespace media

#endif  // MEDIA_CAPTURE_VIDEO_DROIDMEDIA_DROID_VIDEO_CAPTURE_DEVICE_FACTORY_H_
