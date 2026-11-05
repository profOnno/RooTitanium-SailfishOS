// RooTitanium — video capture (getUserMedia) on SailfishOS via droidmedia.
// Copyright (C) 2026 RootGPT. SPDX-License-Identifier: GPL-3.0-or-later

#include "media/capture/video/droidmedia/droid_video_capture_device.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/location.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "media/base/video_frame_metadata.h"
#include "third_party/libyuv/include/libyuv.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

extern "C" {
#include "droidmedia.h"
#include "droidmediacamera.h"
}

namespace media {

namespace {

// Android camera parameters are a flat "key=value;..." string. We do NOT
// rewrite the whole set: only the pairs we care about are replaced, the rest is
// left untouched. A full parse+rebuild would drop duplicate keys (e.g.
// "focus-areas") that the vendor expects.
constexpr char kParamSeparator = ';';

std::vector<std::string> ParamTokens(const std::string& params) {
  return base::SplitString(params, std::string(1, kParamSeparator),
                           base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
}

bool GetParam(const std::string& params,
              const char* key,
              std::string* value) {
  const std::string prefix = std::string(key) + "=";
  for (const std::string& token : ParamTokens(params)) {
    if (base::StartsWith(token, prefix, base::CompareCase::SENSITIVE)) {
      *value = token.substr(prefix.size());
      return true;
    }
  }
  return false;
}

void SetParam(std::string* params,
              const std::string& key,
              const std::string& value) {
  const std::string prefix = key + "=";
  std::vector<std::string> tokens = ParamTokens(*params);
  bool replaced = false;
  for (std::string& token : tokens) {
    if (base::StartsWith(token, prefix, base::CompareCase::SENSITIVE)) {
      token = prefix + value;
      replaced = true;
    }
  }
  if (!replaced) {
    tokens.push_back(prefix + value);
  }
  *params = base::JoinString(tokens, std::string(1, kParamSeparator));
}

bool ParseSize(const std::string& text, int* width, int* height) {
  const size_t x = text.find_first_of("xX");
  if (x == std::string::npos) {
    return false;
  }
  const std::string w = text.substr(0, x);
  const std::string h = text.substr(x + 1);
  return base::StringToInt(w, width) && base::StringToInt(h, height) &&
         *width > 0 && *height > 0;
}

struct SizeChoice {
  int width = 0;
  int height = 0;
};

// The vendor lists the preview sizes it supports; Chromium asks for one. Pick
// the closest by area, with a strong penalty for a different aspect ratio: a
// page asking for 640x480 must not get 640x360 just because it is closer in
// pixel count.
SizeChoice ChooseSize(const std::vector<SizeChoice>& sizes,
                      int requested_width,
                      int requested_height) {
  const double requested_area =
      static_cast<double>(requested_width) * requested_height;
  const double requested_aspect =
      static_cast<double>(requested_width) / requested_height;

  double best_score = -1.0;
  SizeChoice best{requested_width, requested_height};
  for (const SizeChoice& size : sizes) {
    if (size.width < 2 || size.height < 2 || size.width > 1920 ||
        size.height > 1080) {
      continue;
    }
    const double area = static_cast<double>(size.width) * size.height;
    const double aspect = static_cast<double>(size.width) / size.height;
    double score = std::fabs(area - requested_area) / requested_area;
    if (std::fabs(aspect - requested_aspect) > 0.15) {
      score += 4.0;  // wrong aspect: accept only if there is nothing else
    }
    if (best_score < 0.0 || score < best_score) {
      best_score = score;
      best = size;
    }
  }
  return best;
}

// Which preview format to ask for. Only formats the device actually lists are
// considered, because "preview-format" is the one key many vendors validate.
// When the list is missing we try yuv420p: it is the historical Android default
// and, if the vendor dislikes it, it simply ignores it (the first frame will
// tell us).
std::string ChoosePreviewFormat(const std::string& values) {
  const std::vector<std::string> formats =
      base::SplitString(values, ",", base::TRIM_WHITESPACE,
                        base::SPLIT_WANT_NONEMPTY);
  for (const char* preferred : {"yuv420p", "nv12", "yuv420sp"}) {
    for (const std::string& format : formats) {
      if (base::EqualsCaseInsensitiveASCII(format, preferred)) {
        return preferred;
      }
    }
  }
  return std::string();
}

}  // namespace

DroidVideoCaptureDevice::DroidVideoCaptureDevice(int camera_index,
                                                 int orientation,
                                                 bool front_facing)
    : camera_index_(camera_index),
      orientation_(orientation),
      front_facing_(front_facing) {}

DroidVideoCaptureDevice::~DroidVideoCaptureDevice() {
  StopAndDeAllocate();
}

void DroidVideoCaptureDevice::AllocateAndStart(
    const VideoCaptureParams& params,
    std::unique_ptr<Client> client) {
  // Hot restart of the same device: Chromium does this when the track
  // constraints change. The camera really has to be closed before reopening it.
  if (ponte_) {
    StopAndDeAllocate();
  }

  // Per-session state: a failed start must not poison the retry (broken_ would
  // make every future frame bail out) and the counters should describe this
  // session, not the lifetime of the object.
  broken_ = false;
  lock_failed_logged_ = false;
  delivered_ = 0;
  dropped_ = 0;

  requested_size_ = params.requested_format.frame_size;
  if (requested_size_.IsEmpty()) {
    requested_size_ = gfx::Size(640, 480);
  }
  requested_fps_ = params.requested_format.frame_rate;
  if (!(requested_fps_ > 0.0f)) {
    requested_fps_ = 30.0f;
  }
  // Above 60 fps makes no sense on these devices: the vendor delivers what it
  // wants anyway, and a smaller minimum interval filters nothing.
  requested_fps_ = std::min(requested_fps_, 60.0f);

  LOG(INFO) << "droidmedia camera " << camera_index_
            << (front_facing_ ? " (front)" : " (back)") << ": requested "
            << requested_size_.ToString() << " @ " << requested_fps_ << " fps";

  ponte_ = base::MakeRefCounted<Ponte>(this);
  client_ = std::move(client);

  if (!StartCamera()) {
    // StartCamera already called OnError. Keep ponte_ and client_ alive:
    // Chromium will call StopAndDeAllocate(), which cleans them up.
    return;
  }
}

bool DroidVideoCaptureDevice::StartCamera() {
  camera_ = droid_media_camera_connect(camera_index_);
  if (!camera_) {
    FailFrame("connect to the camera failed (busy?)");
    return false;
  }

  queue_ = droid_media_camera_get_buffer_queue(camera_);
  if (!queue_) {
    FailFrame("the camera exposes no buffer queue");
    return false;
  }

  // Camera callbacks: we only care about vendor errors. The others stay null
  // and droidmedia skips them (verified: every invoke is guarded by a pointer
  // check).
  DroidMediaCameraCallbacks camera_cb;
  memset(&camera_cb, 0, sizeof(camera_cb));
  camera_cb.error_cb = [](void* data, int error) {
    DroidVideoCaptureDevice::OnCameraError(data, error);
  };
  droid_media_camera_set_callbacks(camera_, &camera_cb, ponte_.get());

  // ⭐ Frames arrive here. buffer_created keeps a reference to the
  // DroidMediaBuffer (it must be returned with destroy); frame_available
  // delivers the frame and, by returning true, tells droidmedia that WE will
  // release the buffer. Returning false would make droidmedia release it at
  // once, so the frame could be recycled while we are still reading it.
  DroidMediaBufferQueueCallbacks queue_cb;
  memset(&queue_cb, 0, sizeof(queue_cb));
  queue_cb.buffer_created = [](void* data, DroidMediaBuffer* buffer) {
    return DroidVideoCaptureDevice::OnBufferCreated(data, buffer);
  };
  queue_cb.frame_available = [](void* data, DroidMediaBuffer* buffer) {
    return DroidVideoCaptureDevice::OnFrameAvailable(data, buffer);
  };
  queue_cb.buffers_released = [](void* data) {
    DroidVideoCaptureDevice::OnBuffersReleased(data);
  };
  // running_ BEFORE installing the callbacks: this orders the write before
  // their read on the vendor thread (which enters the queue lock), so the first
  // frame is not dropped.
  running_ = true;
  droid_media_buffer_queue_set_callbacks(queue_, &queue_cb, ponte_.get());

  ConfigureCamera();  // not fatal: the first frame will tell what happened

  // Report success BEFORE start_preview: the vendor may deliver the first
  // frame while start_preview is still returning, and Chromium's client expects
  // OnStarted to have happened before frames arrive. If start_preview then
  // fails, OnError follows (a start followed by an error is a normal sequence).
  client_->OnStarted();

  if (!droid_media_camera_start_preview(camera_)) {
    running_ = false;
    FailFrame("start_preview rejected by the vendor");
    return false;
  }
  return true;
}

bool DroidVideoCaptureDevice::ConfigureCamera() {
  char* raw = droid_media_camera_get_parameters(camera_);
  if (!raw) {
    LOG(WARNING) << "droidmedia camera " << camera_index_
                 << ": empty get_parameters, using vendor defaults";
    return false;
  }
  std::string params(raw);
  free(raw);

  int width = requested_size_.width();
  int height = requested_size_.height();
  std::string values;
  if (GetParam(params, "preview-size-values", &values)) {
    std::vector<SizeChoice> sizes;
    for (const std::string& token :
         base::SplitString(values, ",", base::TRIM_WHITESPACE,
                           base::SPLIT_WANT_NONEMPTY)) {
      SizeChoice size;
      if (ParseSize(token, &size.width, &size.height)) {
        sizes.push_back(size);
      }
    }
    if (!sizes.empty()) {
      const SizeChoice chosen = ChooseSize(sizes, width, height);
      width = chosen.width;
      height = chosen.height;
    }
  }
  SetParam(&params, "preview-size",
           base::NumberToString(width) + "x" + base::NumberToString(height));

  if (GetParam(params, "preview-format-values", &values)) {
    const std::string format = ChoosePreviewFormat(values);
    if (!format.empty()) {
      SetParam(&params, "preview-format", format);
    }
  } else {
    SetParam(&params, "preview-format", "yuv420p");
  }

  const bool ok = droid_media_camera_set_parameters(camera_, params.c_str());
  LOG(INFO) << "droidmedia camera " << camera_index_ << ": preview " << width
            << "x" << height
            << (ok ? "" : " (set_parameters REJECTED: vendor default stays)");
  return ok;
}

bool DroidVideoCaptureDevice::HandleFrame(void* buffer_raw) {
  auto* buffer = static_cast<DroidMediaBuffer*>(buffer_raw);
  if (!buffer || !running_ || !client_) {
    return false;
  }

  DroidMediaBufferInfo info;
  memset(&info, 0, sizeof(info));
  droid_media_buffer_get_info(buffer, &info);

  DroidMediaBufferYCbCr ycbcr;
  memset(&ycbcr, 0, sizeof(ycbcr));
  const bool locked = droid_media_buffer_lock_ycbcr(
      buffer, DROID_MEDIA_BUFFER_LOCK_READ, &ycbcr);
  if (locked) {
    ConvertAndDeliver(&info, &ycbcr);
    droid_media_buffer_unlock(buffer);
  } else if (!lock_failed_logged_) {
    lock_failed_logged_ = true;
    LOG(ERROR) << "droidmedia camera " << camera_index_
               << ": lock_ycbcr failed (hal_format=" << info.format
               << "): the buffer format is not CPU-readable";
  }

  // The buffer ALWAYS goes back to the producer, even if conversion failed:
  // without this the camera stops after a few frames (the queue has a finite
  // number of slots).
  droid_media_buffer_release(buffer, nullptr, nullptr);
  return true;
}

void DroidVideoCaptureDevice::ConvertAndDeliver(const void* info_raw,
                                                const void* ycbcr_raw) {
  const auto& info = *static_cast<const DroidMediaBufferInfo*>(info_raw);
  const auto& ycbcr = *static_cast<const DroidMediaBufferYCbCr*>(ycbcr_raw);

  if (broken_) {
    return;
  }

  // ── Geometry ─────────────────────────────────────────────────────────────
  // info.width/height are the vendor buffer; crop_rect is the actually visible
  // part. They are almost always equal, but when they are not, trusting the
  // crop avoids an uninitialized strip on the edge.
  const int buffer_width = static_cast<int>(info.width);
  const int buffer_height = static_cast<int>(info.height);
  int left = 0;
  int top = 0;
  int right = buffer_width;
  int bottom = buffer_height;
  if (info.crop_rect.right > info.crop_rect.left &&
      info.crop_rect.bottom > info.crop_rect.top) {
    left = std::max(0, static_cast<int>(info.crop_rect.left));
    top = std::max(0, static_cast<int>(info.crop_rect.top));
    right = std::min(buffer_width, static_cast<int>(info.crop_rect.right));
    bottom = std::min(buffer_height, static_cast<int>(info.crop_rect.bottom));
  }
  // 4:2:0 works on pairs: an odd crop would swap U and V.
  left &= ~1;
  top &= ~1;
  right &= ~1;
  bottom &= ~1;
  const int width = right - left;
  const int height = bottom - top;
  if (width < 2 || height < 2) {
    FailFrame("degenerate crop rect");
    return;
  }

  if (!ycbcr.y || !ycbcr.cb || !ycbcr.cr) {
    FailFrame("lock_ycbcr returned null pointers");
    return;
  }
  // 1 = separate planes (I420/YV12), 2 = interleaved chroma (NV12/NV21).
  // Any other value is a format libyuv cannot read (Qualcomm/MediaTek tiles):
  // better to say so than to produce a striped image.
  if (ycbcr.chroma_step != 1 && ycbcr.chroma_step != 2) {
    FailFrame("unexpected chroma_step (tiled format?)");
    return;
  }

  const int y_stride = info.stride > 0 ? static_cast<int>(info.stride)
                                       : buffer_width;
  if (y_stride < buffer_width) {
    FailFrame("luma stride shorter than the width");
    return;
  }
  // If the vendor does not report the chroma stride (rare), derive it from the
  // BUFFER width, not the visible one: rows span the whole buffer, not just the
  // crop.
  const int c_stride =
      ycbcr.cstride > 0
          ? static_cast<int>(ycbcr.cstride)
          : (ycbcr.chroma_step == 2 ? 2 * buffer_width : buffer_width / 2);
  if (c_stride < width / 2 * static_cast<int>(ycbcr.chroma_step)) {
    FailFrame("chroma stride shorter than the row");
    return;
  }

  // ── Crop offsets ─────────────────────────────────────────────────────────
  // lock_ycbcr returns the BASE of the buffer: the crop must be added by hand,
  // both in luma (row by row) and in chroma (half resolution, so half a row and
  // half a column). This is exactly the step gecko-camera got wrong: without
  // the offset chroma "slides" and colors drift relative to the video.
  const auto* base_y = static_cast<const uint8_t*>(ycbcr.y);
  const auto* base_u = static_cast<const uint8_t*>(ycbcr.cb);
  const auto* base_v = static_cast<const uint8_t*>(ycbcr.cr);

  const uint8_t* src_y =
      base_y + static_cast<size_t>(top) * y_stride + left;
  const size_t chroma_offset =
      static_cast<size_t>(top / 2) * c_stride +
      static_cast<size_t>(left / 2) * ycbcr.chroma_step;
  const uint8_t* src_u = base_u + chroma_offset;
  const uint8_t* src_v = base_v + chroma_offset;

  // ⚠️ U and V are NOT swapped here for YV12. It looks counterintuitive
  // because in YV12 memory the V plane comes BEFORE U, but cb/cr are not
  // physical offsets: android_ycbcr's contract (and libyuv's own test
  // TESTAPLANARTOP(Android420, NV21, 2, 1, 0, ...)) says cb always points to
  // the Cb/U sample and cr always to Cr/V, whatever the memory order is.
  // Swapping them here would give an image with inverted colors that no log
  // would report: this is the same trap the decoder avoids, but there we
  // compute the pointers ourselves from the format, while here gralloc
  // provides them.

  // ── Conversion ───────────────────────────────────────────────────────────
  const int half_width = width / 2;
  const int half_height = height / 2;
  const size_t y_size = static_cast<size_t>(width) * height;
  const size_t c_size = static_cast<size_t>(half_width) * half_height;
  if (staging_.size() < y_size + 2 * c_size) {
    staging_.resize(y_size + 2 * c_size);
  }
  uint8_t* dst_y = staging_.data();
  uint8_t* dst_u = dst_y + y_size;
  uint8_t* dst_v = dst_u + c_size;

  // Android420ToI420 is the only libyuv function that takes the three planes
  // with their strides AND the chroma_step: it handles I420, YV12, NV12 and
  // NV21 without us having to guess the format. The chroma strides are the
  // buffer's: passing "width/2" would be the second trap (rows going astray on
  // buffers aligned to 128, which are the norm).
  const int rc = libyuv::Android420ToI420(
      src_y, y_stride, src_u, c_stride, src_v, c_stride,
      static_cast<int>(ycbcr.chroma_step), dst_y, width, dst_u, half_width,
      dst_v, half_width, width, height);
  if (rc != 0) {
    FailFrame("libyuv conversion failed");
    return;
  }

  const base::TimeTicks now = base::TimeTicks::Now();
  if (first_frame_time_.is_null()) {
    first_frame_time_ = now;
    // One line, on the first frame: without these numbers, diagnosing a device
    // we do not have in hand remotely is impossible.
    LOG(INFO) << "droidmedia camera " << camera_index_ << ": first frame "
              << buffer_width << "x" << buffer_height << " crop=(" << left
              << "," << top << ")-(" << right << "," << bottom << ")"
              << " stride=" << y_stride << " chroma_stride=" << c_stride
              << " chroma_step=" << ycbcr.chroma_step
              << " hal_format=" << info.format;
  }

  // Honor the requested frame rate: the vendor almost always delivers 30 fps no
  // matter what, and without this filter a 15 fps track would cost twice the
  // CPU for frames Chromium would drop anyway.
  if (!last_delivery_.is_null()) {
    const base::TimeDelta min_interval = base::Microseconds(
        static_cast<int64_t>(1000000.0 / requested_fps_));
    // 10% tolerance: with a strict interval a 30 fps vendor ticking at 33.0 ms
    // would lose every other frame. 27 ms for a 30 fps request still passes.
    const base::TimeDelta tolerance = min_interval / 10;
    if (now - last_delivery_ + tolerance < min_interval) {
      ++dropped_;
      return;
    }
  }
  last_delivery_ = now;

  const VideoCaptureFormat format(gfx::Size(width, height), requested_fps_,
                                  PIXEL_FORMAT_I420);
  client_->OnIncomingCapturedData(
      staging_.data(), static_cast<int>(y_size + 2 * c_size), format,
      gfx::ColorSpace(), orientation_, /*flip_y=*/false, now,
      now - first_frame_time_, /*capture_begin_timestamp=*/std::nullopt,
      /*metadata=*/std::nullopt);
  ++delivered_;
}

void DroidVideoCaptureDevice::FailFrame(const char* reason) {
  if (broken_) {
    return;
  }
  broken_ = true;
  LOG(ERROR) << "droidmedia camera " << camera_index_ << ": " << reason;
  if (client_) {
    client_->OnError(
        VideoCaptureError::
            kVideoCaptureControllerInvalidOrUnsupportedVideoCaptureParametersRequested,
        FROM_HERE, reason);
  }
}

void DroidVideoCaptureDevice::StopAndDeAllocate() {
  if (!ponte_) {
    return;
  }
  // Set running_ = false under the Ponte lock: a vendor error fired by
  // stop_preview/disconnect must not reach FailFrame (camera switch).
  ponte_->StopRunning();
  CloseCamera();
  ponte_->Detach();
  ponte_.reset();
  client_.reset();
  staging_.clear();
  buffers_.clear();
  first_frame_time_ = base::TimeTicks();
  last_delivery_ = base::TimeTicks();
  LOG(INFO) << "droidmedia camera " << camera_index_ << ": stopped ("
            << delivered_ << " frames delivered, " << dropped_
            << " dropped by the fps filter)";
}

void DroidVideoCaptureDevice::CloseCamera() {
  if (camera_) {
    droid_media_camera_stop_preview(camera_);
  }
  // setCallbacks(nullptr) takes the queue's internal lock: when it returns, no
  // frame_available is in progress and none can start. This is the barrier that
  // makes destroying the buffers below safe.
  if (queue_) {
    droid_media_buffer_queue_set_callbacks(queue_, nullptr, nullptr);
  }
  if (camera_) {
    DroidMediaCameraCallbacks empty;
    memset(&empty, 0, sizeof(empty));
    droid_media_camera_set_callbacks(camera_, &empty, nullptr);
  }
  DestroyBuffers();
  if (camera_) {
    droid_media_camera_disconnect(camera_);
    camera_ = nullptr;
  }
  queue_ = nullptr;
}

void DroidVideoCaptureDevice::DestroyBuffers() {
  for (DroidMediaBuffer* buffer : buffers_) {
    droid_media_buffer_destroy(buffer);
  }
  buffers_.clear();
}

// static
bool DroidVideoCaptureDevice::OnBufferCreated(void* data, void* buffer) {
  auto* ponte = static_cast<Ponte*>(data);
  bool kept = false;
  if (ponte) {
    ponte->Run([&](DroidVideoCaptureDevice* self) {
      // The reference droidmedia gives us here is OURS: it must be returned
      // with destroy, not release. Holding it per slot prevents the buffer from
      // disappearing while the producer recycles it.
      self->buffers_.push_back(static_cast<DroidMediaBuffer*>(buffer));
      kept = true;
    });
  }
  return kept;
}

// static
bool DroidVideoCaptureDevice::OnFrameAvailable(void* data, void* buffer) {
  auto* ponte = static_cast<Ponte*>(data);
  bool handled = false;
  if (ponte) {
    ponte->Run([&](DroidVideoCaptureDevice* self) {
      handled = self->HandleFrame(buffer);
    });
  }
  return handled;
}

// static
void DroidVideoCaptureDevice::OnBuffersReleased(void* data) {
  auto* ponte = static_cast<Ponte*>(data);
  if (ponte) {
    ponte->Run([](DroidVideoCaptureDevice* self) { self->DestroyBuffers(); });
  }
}

// static
void DroidVideoCaptureDevice::OnCameraError(void* data, int error) {
  auto* ponte = static_cast<Ponte*>(data);
  if (!ponte) {
    return;
  }
  ponte->Run([error](DroidVideoCaptureDevice* self) {
    // The vendor reports CAMERA_ERROR_EVICTED (2) while a session is being
    // closed: stop_preview/disconnect make the HAL evict the client. That is
    // expected on teardown and must not reach Chromium's client, or a camera
    // switch looks like a fatal capture error (black/frozen preview until the
    // track is restarted). Only an error while running_ is a real failure.
    if (!self->running_) {
      LOG(INFO) << "droidmedia camera " << self->camera_index_
                << ": vendor error " << error
                << " during teardown (ignored)";
      return;
    }
    LOG(ERROR) << "droidmedia camera: vendor error: " << error;
    self->FailFrame("error reported by the camera");
  });
}

}  // namespace media
