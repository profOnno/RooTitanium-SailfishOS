// RooTitanium — video capture factory via droidmedia (SailfishOS).
// Copyright (C) 2026 RootGPT. SPDX-License-Identifier: GPL-3.0-or-later

#include "media/capture/video/droidmedia/droid_video_capture_device_factory.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "media/capture/video/droidmedia/droid_video_capture_device.h"
#include "ui/gfx/geometry/size.h"

extern "C" {
#include "droidmedia.h"
#include "droidmediacamera.h"
}

namespace media {

namespace {

// Path of the ANDROID library (not glibc): it can only be loaded through the
// libhybris linker, which on every SFOS port is libhybris-common.so.1.
constexpr char kHybrisCommon[] = "libhybris-common.so.1";
constexpr char kDroidMedia[] = "libdroidmedia.so";

// device_id prefix. It must stay stable: Chromium keeps it between enumeration
// and CreateDevice (and the page sees it in getSettings()).
constexpr char kDeviceIdPrefix[] = "droidmedia:";

// Do NOT open the camera here. GetDevicesInfo() runs on every
// enumerateDevices(), and Jitsi/Meet call it while a call is already live.
// Opening the HAL just to read preview-size-values evicts the active session
// (CAMERA_ERROR_EVICTED, 0 frames), which is exactly the camera-switch failure
// seen on the Xperia 10 V. Advertise the resolutions verified on that device;
// the real size is negotiated in ConfigureCamera().
VideoCaptureFormats QueryPreviewFormats(int camera_index) {
  (void)camera_index;
  VideoCaptureFormats formats;
  formats.emplace_back(gfx::Size(640, 480), 30.0f, PIXEL_FORMAT_I420);
  formats.emplace_back(gfx::Size(1280, 720), 30.0f, PIXEL_FORMAT_I420);
  return formats;
}

}  // namespace

// static
bool DroidVideoCaptureDeviceFactory::IsSupported() {
  static const bool supported = [] {
    // Test switch, twin of the decoder's RT_DROIDMEDIA:
    // RT_DROIDMEDIA_CAMERA=0 turns off ONLY this capture path and leaves the
    // rest of the engine identical (the only honest comparison when a symptom
    // must be attributed to the camera or to something else).
    if (const char* env = getenv("RT_DROIDMEDIA_CAMERA")) {
      if (env[0] == '0') {
        LOG(INFO) << "droidmedia camera: disabled by "
                     "RT_DROIDMEDIA_CAMERA=0";
        return false;
      }
    }

    void* hybris = dlopen(kHybrisCommon, RTLD_LAZY);
    if (!hybris) {
      DVLOG(1) << "droidmedia camera: " << kHybrisCommon
               << " missing, staying on V4L2";
      return false;
    }
    using AndroidDlopen = void* (*)(const char*, int);
    using AndroidDlsym = void* (*)(void*, const char*);
    auto android_dlopen =
        reinterpret_cast<AndroidDlopen>(dlsym(hybris, "android_dlopen"));
    auto android_dlsym =
        reinterpret_cast<AndroidDlsym>(dlsym(hybris, "android_dlsym"));
    if (!android_dlopen || !android_dlsym) {
      DVLOG(1) << "droidmedia camera: libhybris without android_dlopen/dlsym";
      return false;
    }

    void* handle = android_dlopen(kDroidMedia, RTLD_NOW);
    if (!handle) {
      DVLOG(1) << "droidmedia camera: " << kDroidMedia << " not loadable";
      return false;
    }

    // 🔴 Every function we call must be in this list. The shim resolves symbols
    // on first call and, if one is missing, does assert()+abort(): a port with
    // an older droidmedia would kill the browser instead of falling back to
    // V4L2.
    static constexpr const char* kNeeded[] = {
        // ⚠️ "_droid_media_init", not "droid_media_init": the latter is a
        // convenience of the STATIC shim (hybris.c) that ends up in our binary,
        // not a symbol exported by the Android library. Checking the wrong name
        // here would always answer "not supported".
        "_droid_media_init",
        "droid_media_camera_get_number_of_cameras",
        "droid_media_camera_get_info",
        "droid_media_camera_connect",
        "droid_media_camera_disconnect",
        "droid_media_camera_get_buffer_queue",
        "droid_media_camera_set_callbacks",
        "droid_media_camera_get_parameters",
        "droid_media_camera_set_parameters",
        "droid_media_camera_start_preview",
        "droid_media_camera_stop_preview",
        "droid_media_buffer_queue_set_callbacks",
        "droid_media_buffer_get_info",
        "droid_media_buffer_lock_ycbcr",
        "droid_media_buffer_unlock",
        "droid_media_buffer_release",
        "droid_media_buffer_destroy",
    };
    for (const char* symbol : kNeeded) {
      if (!android_dlsym(handle, symbol)) {
        LOG(INFO) << "droidmedia camera: missing symbol " << symbol
                  << ", staying on V4L2";
        return false;
      }
    }

    // From here on the calls are legal. droid_media_init() starts the binder
    // thread; it runs once per process and is never torn down.
    if (!droid_media_init()) {
      LOG(WARNING) << "droidmedia camera: droid_media_init() failed";
      return false;
    }
    const int cameras = droid_media_camera_get_number_of_cameras();
    if (cameras <= 0) {
      LOG(INFO) << "droidmedia camera: no visible camera, staying on V4L2";
      return false;
    }
    LOG(INFO) << "droidmedia camera: " << cameras << " camera(s) found";
    return true;
  }();
  return supported;
}

DroidVideoCaptureDeviceFactory::DroidVideoCaptureDeviceFactory() = default;
DroidVideoCaptureDeviceFactory::~DroidVideoCaptureDeviceFactory() = default;

VideoCaptureErrorOrDevice DroidVideoCaptureDeviceFactory::CreateDevice(
    const VideoCaptureDeviceDescriptor& device_descriptor) {
  if (!base::StartsWith(device_descriptor.device_id, kDeviceIdPrefix,
                        base::CompareCase::SENSITIVE)) {
    return VideoCaptureErrorOrDevice(
        VideoCaptureError::kVideoCaptureSystemDeviceIdNotFound);
  }

  int camera_index = -1;
  if (!base::StringToInt(
          device_descriptor.device_id.substr(strlen(kDeviceIdPrefix)),
          &camera_index) ||
      camera_index < 0) {
    return VideoCaptureErrorOrDevice(
        VideoCaptureError::kVideoCaptureSystemDeviceIdNotFound);
  }

  DroidMediaCameraInfo info;
  memset(&info, 0, sizeof(info));
  if (!droid_media_camera_get_info(&info, camera_index)) {
    return VideoCaptureErrorOrDevice(
        VideoCaptureError::kVideoCaptureSystemDeviceIdNotFound);
  }

  return VideoCaptureErrorOrDevice(std::make_unique<DroidVideoCaptureDevice>(
      camera_index, info.orientation,
      info.facing == DROID_MEDIA_CAMERA_FACING_FRONT));
}

void DroidVideoCaptureDeviceFactory::GetDevicesInfo(
    GetDevicesInfoCallback callback) {
  std::vector<VideoCaptureDeviceInfo> devices;
  if (IsSupported()) {
    const int count = droid_media_camera_get_number_of_cameras();
    for (int index = 0; index < count; ++index) {
      DroidMediaCameraInfo info;
      memset(&info, 0, sizeof(info));
      if (!droid_media_camera_get_info(&info, index)) {
        continue;
      }
      const bool front = info.facing == DROID_MEDIA_CAMERA_FACING_FRONT;

      VideoCaptureDeviceDescriptor descriptor(
          front ? "Front camera" : "Back camera",
          std::string(kDeviceIdPrefix) + base::NumberToString(index),
          /*model_id=*/std::string(), VideoCaptureApi::LINUX_V4L2_SINGLE_PLANE,
          VideoCaptureControlSupport(),
          VideoCaptureTransportType::OTHER_TRANSPORT,
          front ? VideoFacingMode::MEDIA_VIDEO_FACING_USER
                : VideoFacingMode::MEDIA_VIDEO_FACING_ENVIRONMENT);

      VideoCaptureDeviceInfo device(descriptor);
      device.supported_formats = QueryPreviewFormats(index);
      devices.push_back(device);
    }
  }
  std::move(callback).Run(std::move(devices));
}

}  // namespace media
