# Video capture (getUserMedia) via droidmedia — status

Canonical sources for `droid_video_capture_device.{h,cc}` and
`droid_video_capture_device_factory.{h,cc}`. They enter the build tree through
`../scripts/apply-droidcamera.sh`, which copies them into
`media/capture/video/droidmedia/` and wires `media/capture/BUILD.gn` plus
`media/capture/video/create_video_capture_device_factory.cc`.

## Why droidmedia and not V4L2

On SailfishOS the camera is not a useful `/dev/videoN`: the Android HAL talks to
`cameraserver` over binder and frames arrive from a `BufferQueue`. V4L2 only
works on devices that expose the camera through a Linux video driver (not the
case for hybris ports). droidmedia is the same layer used by `gecko-camera`
(Jolla's browser) and `gst-droid`, and it exists on every port: one
implementation, all devices.

The factory replaces `VideoCaptureDeviceFactoryLinux` **only when**
`IsSupported()` says yes:

1. `libhybris-common.so.1` loads and exposes `android_dlopen`/`android_dlsym`;
2. `libdroidmedia.so` loads through the Android linker;
3. **every** symbol we use exists in the device's library;
4. `droid_media_init()` succeeds and
   `droid_media_camera_get_number_of_cameras() > 0`.

If any of the four fails we stay on V4L2, so the same binary does not regress on
a native port or with an old droidmedia.

⚠️ In step 3 the right symbol is **`_droid_media_init`**, not
`droid_media_init`: the latter is a convenience of the *static shim*
(`hybris.c`) that ends up inside our binary, not a symbol exported by
`libdroidmedia.so`. Checking the wrong name here would always answer "not
supported".

## The hard part: reading the vendor buffer

A preview frame is **not** a contiguous I420 image.
`droid_media_buffer_get_info()` gives the buffer `width`/`height`, `stride` and
a `crop_rect`; the three planes come from
`droid_media_buffer_lock_ycbcr()`, which returns:

```c
typedef struct {
  void *y, *cb, *cr;
  size_t ystride, cstride, chroma_step;
} DroidMediaBufferYCbCr;
```

The traps, each of which has cost somebody a bug before:

1. **Stride is not width.** Buffers are aligned (typically 16, 32 or 128 bytes,
   depending on the vendor). Copying `width` bytes per row produces a
   diagonally striped image.
2. **The crop must be added by hand.** `lock_ycbcr` returns the buffer base, not
   the crop origin. Luma: `top * ystride + left`. Chroma, at half resolution:
   `(top/2) * cstride + (left/2) * chroma_step`.
3. **`cb` and `cr` are semantic, not physical offsets.** `cb` always points to
   the Cb/U sample and `cr` always to Cr/V, even when V comes first in memory
   (YV12) or the two are interleaved (NV12/NV21). libyuv's own test
   `TESTAPLANARTOP(Android420, NV21, 2, 1, 0, …)` pins this down: for NV21
   `src_u` is at `base+1` and `src_v` at `base`. They must **not** be swapped
   based on pointer order: that yields an image with inverted colors that no
   log reports.
4. **`chroma_step` is the truth.** 1 = separate planes, 2 = interleaved chroma.
   That is why we use `libyuv::Android420ToI420`, the only libyuv function that
   takes the three planes with their strides *and* the `chroma_step`, and picks
   I420, YV12, NV12 or NV21 by itself. No HAL format matching is needed.
5. **Tiled formats.** Some vendors deliver proprietary tiles
   (`QOMX_COLOR_Format…Tile2m8ka`, MediaTek formats). In that case
   `lock_ycbcr` fails or `chroma_step` is not 1/2: we report the error and stop
   instead of producing a blocky image. A hand-written de-tiler would be code
   we cannot verify on hardware we do not have.

The offset/chroma arithmetic was checked separately with a Python model over
I420, YV12, NV12 and NV21, with an even crop and padded stride: all four
layouts give the expected U/V sample.

## Buffer lifetime

`droidmedia` delivers frames on its own thread. The contract is:

- `buffer_created(data, buffer)` → gives us **a reference of our own**, to be
  returned with `droid_media_buffer_destroy()`. We keep it in `buffers_`.
- `frame_available(data, buffer)` → if we return `true`, droidmedia does **not**
  release the slot: we must do it with `droid_media_buffer_release()`. If we
  returned `false` it would release it itself, but the frame could be recycled
  while we are reading it. So: copy, `unlock`, `release`, `return true`.
- `buffers_released(data)` → the queue invalidated its slots: our references
  must be destroyed.

Every frame is copied into a reusable I420 staging buffer and delivered with
`Client::OnIncomingCapturedData()`. That is one copy more than zero-copy, but it
is the same path V4L2 uses and it keeps the code verifiable.

In `StopAndDeAllocate()`:

1. `droid_media_camera_stop_preview()`;
2. `droid_media_buffer_queue_set_callbacks(queue, NULL, NULL)` — takes the
   queue's internal lock: when it returns, no `frame_available` is running and
   none can start. This is the barrier that makes destruction safe;
3. camera callbacks cleared;
4. `droid_media_buffer_destroy()` on the held references;
5. `droid_media_camera_disconnect()`;
6. `Ponte::Detach()` + `client_` reset.

`Ponte` is the same scheme as the decoder: a lock plus a pointer nulled before
teardown, so an in-flight callback either arrives while we are alive or touches
nothing.

## Status

**Written 2026-11-03. Compiled on 2026-11-04 into QtWebEngine 6.140
(aarch64); first device test on the Xperia 10 V on 2026-11-04.**

### First device test (2026-11-04)

`getUserMedia` works, both cameras open, and the image is upright with the
right colours. The report (`tools/camera-test`) shows:

- back camera ~30 fps, front camera ~15–16 fps; 1280×720 and 480×640 work;
- stop→open switching in both directions and repeated open/close pass;
- `applyConstraints({deviceId})` fails with `OverconstrainedError` (Chromium
  does not switch the device of a live track that way; expected, not a driver
  bug);
- the only native errors were `vendor error: 2` = `CAMERA_ERROR_EVICTED`,
  emitted by the HAL **while the session is being closed**.

That last point was a real bug: `OnCameraError()` called `FailFrame()` even
during teardown, so a normal eviction notice was reported to Chromium as a
fatal capture error. During a camera switch that can leave the new preview
black/frozen until the track is restarted. Fixed by ignoring vendor errors when
`running_` is false (and logging them as teardown at INFO). Rebuilt as
`harbour-rootitanium-2.9.2-4`; needs a retest with the switch stress test.

The launcher now adds `--enable-logging=stderr --log-level=0` when
`/home/rootitanium/DEBUG` exists, so the next log also contains `requested`,
`preview`, `first frame` and `stopped`, not just the ERROR lines.

### Jitsi / Meet camera switch (2026-11-05)

The second device log (with INFO enabled) showed the real failure:

- `GetDevicesInfo()` opened **both** cameras to read `preview-size-values`.
  Jitsi calls `enumerateDevices()` during a call, so each enumeration
  opened/closed the HAL and evicted the live session
  (`CAMERA_ERROR_EVICTED`, 0 frames). The next `getUserMedia` for the back
  camera then started, got error 2 after ~150 ms and delivered 0 frames:
  "Back camera doesn't work. Front works".
- The first front-camera switch also got 0 frames, then a retry worked; the
  teardown error was racing with `running_ = false`, which was written without
  the Ponte lock.

Fixes in 2.9.2-5/6:

- `QueryPreviewFormats()` no longer opens the camera. Enumeration can no longer
  evict a live call. Before the first capture it advertises two universal sizes
  (640x480, 1280x720); `ConfigureCamera()` then parses the vendor's real
  `preview-size-values` and `CachePreviewFormats()` stores them, so
  `GetDevicesInfo()` publishes the real capabilities from the next enumeration
  on — on any phone, without an extra open.
- `StopAndDeAllocate()` calls `Ponte::StopRunning()`, which sets `running_ =
  false` while holding the callback lock, so an in-flight teardown error is
  ignored instead of calling `FailFrame()`.

The audio side was tested too: audio-only capture works (peak −5.2 dBFS, 3 s
WebM clip); audio+video opens both tracks. See `tools/camera-test`.

Retest 2026-11-05 with 2.9.2-6: four Jitsi front↔back switches, **0 vendor
errors and 0 zero-frame sessions**. Adaptive formats worked (back camera
`getCapabilities` max 1920×1080 after the first capture, front 1280×720).
Microphone: audio-only peak −12.4 dBFS, combined audio+video peak −20.8 dBFS
(30% silent, the user was speaking), 48.6 KB WebM clip.

The first real compile found two errors, both fixed:

- `base::EqualsCaseInsensitive` no longer exists in Chromium 140 →
  `EqualsCaseInsensitiveASCII` (the values are ASCII format names);
- `ParseSize` was defined in both camera .cc files, and `capture_lib` is a
  jumbo target, so they land in the same translation unit → the factory's
  helper is now `ParsePreviewSize`.

The engine builds cleanly (`libQt6WebEngineCore.so.6.140.0`, 275 MB) and the
installed library contains the camera log strings. Next: bundle + device test
(see `Documentation/TASK-camera.md`).

## Diagnostics

On the first frame **one line** reports the vendor's real numbers:

```
droidmedia camera 0: first frame 1280x720 crop=(0,0)-(1280,720) \
  stride=1280 chroma_stride=1280 chroma_step=2 hal_format=21
```

Striped video → wrong stride (should not happen: gralloc provides it). Inverted
colors → `cb`/`cr` swapped somewhere (not here: see above). No frames at all →
`lock_ycbcr` failed or `start_preview` was rejected, and the log says so.

`RT_DROIDMEDIA_CAMERA=0` disables **only** this capture path and leaves the rest
of the engine identical: the only honest comparison when a symptom must be
attributed to the camera or to something else.

## Known limits (deliberate, for this first iteration)

- **No still photos**: `TakePhoto`/`GetPhotoState` remain Chromium defaults (the
  callback is dropped). `getUserMedia` works, `<input capture>`/`ImageCapture`
  do not.
- **No torch, zoom, focus or face detection**: the API exists, but exposing it
  means guessing vendor parameters.
- **No front-camera mirroring**: we pass `flip_y=false` and the sensor
  orientation, like Chromium does on Android; mirroring belongs to the context
  (CSS/UI), not to the device.
- **No output format negotiation**: we always deliver I420.
  `requested_format.pixel_format` is ignored (the client converts).
- **Frame rate**: filtered on our side to `requested_frame_rate`; the vendor
  still delivers ~30 fps.
- **`GetDevicesInfo` opens the camera** once to read the real resolutions. If
  another app has it, the device is still listed with two standard formats.
