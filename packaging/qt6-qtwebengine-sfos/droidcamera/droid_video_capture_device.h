// RooTitanium — video capture (getUserMedia) on SailfishOS via droidmedia.
// Copyright (C) 2026 RootGPT. SPDX-License-Identifier: GPL-3.0-or-later
//
// Why not V4L2, which is what Chromium uses on Linux: on SailfishOS the camera
// is not a usable /dev/videoN. The Android HAL talks to cameraserver over
// binder and frames arrive from a BufferQueue; droidmedia is the same layer
// used by Jolla's own browser (gecko-camera) and gst-droid, and it exists on
// EVERY port. One implementation, all devices.
//
// The hard part is not opening the camera: it is reading the buffer. It is not
// a contiguous I420 frame. It has a stride that does not match the width, a
// crop rect, and chroma that may be planar (I420/YV12) or interleaved
// (NV12/NV21). Reading it goes through droid_media_buffer_lock_ycbcr(), which
// returns the three plane pointers and the vendor's real chroma_step: see
// HandleFrame() in the .cc.

#ifndef MEDIA_CAPTURE_VIDEO_DROIDMEDIA_DROID_VIDEO_CAPTURE_DEVICE_H_
#define MEDIA_CAPTURE_VIDEO_DROIDMEDIA_DROID_VIDEO_CAPTURE_DEVICE_H_

#include <stdint.h>

#include <memory>
#include <vector>

#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "media/capture/video/video_capture_device.h"

// Only droidmedia's OPAQUE types can be forward-declared: its headers define
// them as `typedef struct _DroidMediaBuffer DroidMediaBuffer;` and so on. The
// ANONYMOUS structs (DroidMediaBufferInfo, DroidMediaBufferYCbCr) stay confined
// to the .cc: in jumbo builds a droidmedia header inside media/ would be fused
// with half of media/capture (same lesson as the decoder, 17/09).
struct _DroidMediaBuffer;
struct _DroidMediaBufferQueue;
struct _DroidMediaCamera;

namespace media {

class DroidVideoCaptureDevice : public VideoCaptureDevice {
 public:
  // `camera_index` is the index droidmedia passes to the HAL (0 = back camera
  // on 99% of devices, but do not trust it: get_info reports the real facing).
  // `orientation` is the clockwise angle in degrees between the sensor and the
  // upright device: the same number Chromium expects in OnIncomingCapturedData.
  DroidVideoCaptureDevice(int camera_index,
                          int orientation,
                          bool front_facing);
  DroidVideoCaptureDevice(const DroidVideoCaptureDevice&) = delete;
  DroidVideoCaptureDevice& operator=(const DroidVideoCaptureDevice&) = delete;
  ~DroidVideoCaptureDevice() override;

  // VideoCaptureDevice
  void AllocateAndStart(const VideoCaptureParams& params,
                        std::unique_ptr<Client> client) override;
  void StopAndDeAllocate() override;
  // With OnIncomingCapturedData we do not reserve buffers from the client:
  // every frame is a copy into the buffer the client reserves itself. There is
  // nothing to invalidate.
  void InvalidateBuffers() override {}

 private:
  // ⭐ Bridge between the vendor thread and our object lifetime. droidmedia
  // callbacks arrive on its own thread: without this, an in-flight frame can
  // find the device already destroyed. Detach() nulls the pointer BEFORE the
  // camera and buffers are closed, and waits for any callback in progress.
  class Ponte : public base::RefCountedThreadSafe<Ponte> {
   public:
    explicit Ponte(DroidVideoCaptureDevice* device) : device_(device) {}
    Ponte(const Ponte&) = delete;
    Ponte& operator=(const Ponte&) = delete;

    void Detach() {
      base::AutoLock lock(lock_);
      device_ = nullptr;
    }

    template <typename F>
    void Run(F function) {
      base::AutoLock lock(lock_);
      if (device_) {
        function(device_);
      }
    }

   private:
    friend class base::RefCountedThreadSafe<Ponte>;
    ~Ponte() = default;

    base::Lock lock_;
    DroidVideoCaptureDevice* device_ GUARDED_BY(lock_) = nullptr;
  };

  // Opens the camera, sets preview-size/preview-format and starts the preview.
  // Returns false (and calls OnError) if the device refuses to cooperate.
  bool StartCamera();
  // Reads the current parameters and overwrites the size and format we know
  // how to convert. Not fatal if the vendor rejects them: the first frame will
  // still tell the truth.
  bool ConfigureCamera();
  // Runs on the vendor thread, under the Ponte lock.
  bool HandleFrame(void* buffer);

  // Converts one YCbCr frame into the staging I420 buffer and delivers it to
  // the client. `info` and `ycbcr` are ANONYMOUS droidmedia structs: that is
  // why the method is private and takes `void*` in the header (see above).
  void ConvertAndDeliver(const void* info, const void* ycbcr);

  // Unrecoverable error: report it ONCE and stop capturing.
  void FailFrame(const char* reason);

  static bool OnBufferCreated(void* data, void* buffer);
  static bool OnFrameAvailable(void* data, void* buffer);
  static void OnBuffersReleased(void* data);
  static void OnCameraError(void* data, int error);

  // Closes everything StartCamera opened. Idempotent.
  void CloseCamera();
  void DestroyBuffers();

  const int camera_index_;
  const int orientation_;
  const bool front_facing_;

  // Requested by Chromium, read in AllocateAndStart. The size actually
  // negotiated is chosen by the vendor and shows up in the first frame.
  gfx::Size requested_size_;
  float requested_fps_ = 30.0f;

  // Everything below is protected by the Ponte lock: it is touched both by the
  // Chromium thread (start/stop) and by the vendor thread (frames).
  _DroidMediaCamera* camera_ = nullptr;
  _DroidMediaBufferQueue* queue_ = nullptr;
  std::unique_ptr<Client> client_;
  std::vector<_DroidMediaBuffer*> buffers_;
  std::vector<uint8_t> staging_;  // reusable I420 scratch buffer
  bool running_ = false;
  bool broken_ = false;
  bool lock_failed_logged_ = false;
  base::TimeTicks first_frame_time_;
  base::TimeTicks last_delivery_;
  int delivered_ = 0;
  int dropped_ = 0;

  scoped_refptr<Ponte> ponte_;
};

}  // namespace media

#endif  // MEDIA_CAPTURE_VIDEO_DROIDMEDIA_DROID_VIDEO_CAPTURE_DEVICE_H_
