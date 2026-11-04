# Camera / getUserMedia via droidmedia

**Opened 2026-11-03.** Continues the RooTitanium engine work: the browser now
speaks droidmedia for video decoding (`TASK-decodifica-video.md`); this task
does the same for the camera.

## Why this task exists

On SailfishOS, `navigator.mediaDevices.getUserMedia({video: true})` returns no
camera. QtWebEngine 6.140 bundles Chromium 140, whose Linux capture backend is
built around V4L2 only: `media/capture/BUILD.gn` compiles
`video/linux/video_capture_device_factory_linux.cc` and the V4L2 delegate on
every Linux build, and `CreateVideoCaptureDeviceFactory()` returns that factory.
Enumerating `/dev/video*` finds nothing useful on a hybris port, because the
camera lives behind Android's `cameraserver` and its `BufferQueue`, not behind a
Linux video driver.

The same problem has already been solved twice in the Sailfish ecosystem:
`gecko-camera` (Jolla's browser) and `gst-droid` (`droidcamsrc`) both use
`droidmedia`. That is the portable choice: one implementation for every device.

## What was verified (do not re-research)

- QtWebEngine 6.140 is built from Chromium 140.0.7339.264; the capture factory
  hook is `CreatePlatformSpecificVideoCaptureDeviceFactory()` in
  `media/capture/video/create_video_capture_device_factory.cc`.
- `media/capture/BUILD.gn` already depends on `//third_party/libyuv`, so the
  YCbCr → I420 conversion needs no new dependency.
- droidmedia's camera API (`droidmediacamera.h`) is the same on every port:
  `droid_media_camera_connect(index)`, `..._get_buffer_queue()`,
  `..._set_parameters()`, `..._start_preview()`, plus the buffer-queue callbacks
  `buffer_created` / `frame_available` / `buffers_released`.
- **`frame_available` returning `true` means "I release the buffer myself"**
  (`private.cpp`, `_DroidMediaBufferQueue::frameAvailable`). Returning `false`
  makes droidmedia release the slot immediately, so the frame could be recycled
  while the consumer is still reading it.
- `buffer_created` hands over a reference that must be returned with
  `droid_media_buffer_destroy()`, not `release()`.
- `android_ycbcr` (hence `DroidMediaBufferYCbCr`) uses **semantic** `cb`/`cr`:
  `cb` is Cb/U and `cr` is Cr/V regardless of physical order. libyuv's
  `TESTAPLANARTOP(Android420, NV21, 2, 1, 0, …)` confirms it. Swapping by
  pointer order (as one might for YV12) is wrong here.
- The droidmedia static shim exports `droid_media_init()`, but the Android
  library exports **`_droid_media_init`**: the symbol check must look for the
  latter, or it always fails.

## Design

```
CreateVideoCaptureDeviceFactory()
  └─ DroidVideoCaptureDeviceFactory::IsSupported()   (dlopen + symbols + cameras)
       ├─ no  → VideoCaptureDeviceFactoryLinux       (unchanged)
       └─ yes → DroidVideoCaptureDeviceFactory
                  ├─ GetDevicesInfo: droidmedia camera count/facing/orientation
                  └─ CreateDevice:   DroidVideoCaptureDevice
                        AllocateAndStart → connect, set preview-size/format,
                                           start_preview, OnStarted
                        frame_available  → lock_ycbcr → Android420ToI420
                                           → OnIncomingCapturedData
                        StopAndDeAllocate→ stop_preview, callbacks=NULL,
                                           destroy refs, disconnect
```

Everything is wired by `packaging/qt6-qtwebengine-sfos/scripts/apply-droidcamera.sh`
(idempotent, like `apply-droidmedia.sh`); the sources stay canonical under
`packaging/qt6-qtwebengine-sfos/droidcamera/` and are copied into the tree.

## The buffer-layout question (the original bug report)

The starting point was the hint about `DroidMediaBufferYCbCr` and the
`mStride`/`mSkip`/`mWidth`/`mHeight` of the Cb/Cr planes. In fact:

- the vendor may interleave chroma (`chroma_step == 2`, NV12/NV21) or keep it
  planar (`chroma_step == 1`, I420/YV12);
- `ystride`/`cstride` are the real row strides and are not the visible width;
- `crop_rect` may be a sub-rectangle; the base pointers do not account for it;
- `cb`/`cr` are already semantic, so no U/V swap is needed even for YV12.

We therefore do not reimplement the conversion: `libyuv::Android420ToI420` takes
all of this as parameters. The arithmetic (crop offset in chroma = half row and
half column, times `chroma_step`) was checked with a standalone model over all
four layouts before writing the C++.

## Phases

### Phase 0 — research and wiring: ✅ done (2026-11-03)
- API and lifetime verified against `sailfishos/droidmedia`,
  `sailfishos/gst-droid`, `sailfishos/gecko-camera`, Chromium 140 and libyuv.
- Sources, GN wiring and factory hook written.

### Phase 1 — compile: ✅ done (2026-11-04)
QtWebEngine 6.140 (aarch64) built and installed with the camera code in
`libQt6WebEngineCore.so.6.140.0`. Two compile errors surfaced and were fixed:
`base::EqualsCaseInsensitive` was removed in Chromium 140 (use
`EqualsCaseInsensitiveASCII`), and the two camera .cc files share a jumbo
translation unit, so their `ParseSize` helpers had to be renamed. The
`webengine-smoke` binary against the installed engine also builds.

### Phase 2 — device test: ⬜ to do
Test page (local file, no network):

```html
<video autoplay playsinline muted></video>
<script>
navigator.mediaDevices.getUserMedia({video: {width: 1280, height: 720}})
  .then(s => document.querySelector('video').srcObject = s)
  .catch(e => console.error(e));
</script>
```

Check, in this order:

1. `enumerateDevices()` lists Back/Front camera with `droidmedia:` ids;
2. the log shows `N camera(s) found` and `preview WxH`;
3. the log shows exactly one `first frame ... stride=... chroma_step=...` line;
4. the image is upright, not striped, colors are not swapped, and there is no
   chroma shift at the edges (crop offset);
5. `RT_DROIDMEDIA_CAMERA=0` makes the camera disappear and V4L2 behave as before
   (fallback sanity check);
6. switching front/back and stopping/restarting the track does not leak or hang.

### Phase 3 — performance / zero-copy: later
One copy per frame (`lock_ycbcr` → I420 staging → shared memory). The real win
would be delivering the vendor buffer as a GPU-backed `VideoFrame`, the same
open problem as `TASK-zero-copy-video.md`.

### Phase 4 — photos and controls: later
`TakePhoto`, torch, zoom and focus. The API exists; exposing it means agreeing
on vendor parameters, and the Xperia 10 V torch quirk (see the flashlight
investigation in the earlier session) makes this device-specific.

## Open points

- **Tiled preview formats.** If a vendor only offers a tiled preview format,
  `lock_ycbcr` fails and the camera reports an error. We would need either a
  different preview format or a de-tiler; the log will say which format arrived.
- **Permissions/sandbox.** The app must be allowed to open the camera (Sailfish
  permission `Camera`, and the user in the right group). If `connect` fails
  while another app holds the camera, that is expected; the error says so.
- **droidmedia version on the target.** `IsSupported()` checks symbols, so an
  older droidmedia degrades to V4L2 instead of aborting, but the camera will
  then simply be absent. The first log line tells which step failed.
