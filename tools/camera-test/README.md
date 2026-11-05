# RooTitanium camera test page

A self-contained page to characterise `getUserMedia` and **camera switching**
on the Xperia 10 V, plus a tiny Python HTTPS server that stores the reports.

The page records, for every open/switch:

- device list before and after permission (labels, `deviceId`, `groupId`);
- `getUserMedia` time, time to first frame, negotiated size;
- `track.getSettings()` / `getCapabilities()` (facingMode, deviceId, frameRate…);
- 10 canvas samples: mean luma, chroma, black/frozen detection, unique frames;
- presented-frame rate (`requestVideoFrameCallback`);
- a 320×240 JPEG snapshot of the live frame (so orientation, colours and
  striping can be checked by eye);
- `applyConstraints({deviceId})` switching, opening a second camera while the
  first is live, and repeated open/close cycles;
- all `track` `ended`/`mute`/`unmute` events and every JS error.

Nothing is uploaded anywhere except the local server you run (or the JSON file
you download).

## Run it

On a machine the phone can reach on the same Wi-Fi (the LXD **host** is the easy
choice; the container itself sits behind NAT):

```sh
cd tools/camera-test
python3 server.py
```

It prints something like:

```
  camera test:  https://<server-ip>:8443/
  reports:      /.../tools/camera-test/reports
```

Reports are written next to `server.py` (`./reports`) and the folder is created
at startup. They are also browsable from the phone at
`https://<address>:8443/reports/` (the JSON and every snapshot). To put them
somewhere else — e.g. a directory the host already shares — use
`--reports-dir /path/to/reports`.

On the phone open that URL, accept the self-signed certificate warning once.
`getUserMedia` requires a secure context; plain HTTP will not work (except on
`localhost`, which the phone is not).

The server also prints a **QR code** of that URL in the terminal (via
`qrencode`; if it is missing it falls back to `python3-qrcode`/`segno`). Scan it
with the phone camera. Use `--no-qr` to skip it, or install the tool with
`sudo apt install qrencode` / `pip install qrcode` if the server says no QR
backend is available.

If the auto-detected LAN address is not the one the phone should use (multiple
interfaces, port forwarding, VPN), pass the exact address with
`--url https://<address>:8443/`; both the printed line and the QR use it.

You can also copy `index.html` to any HTTPS host you already have. Then use
**Download JSON** instead of **Send to server** (the page has no other network
access).

### If the phone shows `ERR_CERT_AUTHORITY_INVALID`

QtWebEngine rejects the self-signed certificate, so the page never loads. From
RPM **2.9.2-3** there is a development marker that appends
`--ignore-certificate-errors` to Chromium:

```sh
touch /home/rootitanium/ALLOW_BAD_CERT   # then restart RooTitanium
# ... run the test ...
rm /home/rootitanium/ALLOW_BAD_CERT      # do not leave it: it disables TLS
                                         # validation for ALL sites
```

The marker only makes the browser ignore the warning; it does not install the
certificate. If you prefer not to weaken the browser, use a certificate the
phone already trusts (e.g. a Cloudflare Tunnel URL) or host `index.html` on an
existing HTTPS site and send the downloaded JSON.

The `tools/camera-test` server certificate is self-signed and generated on the
machine that runs it; its public key is not in the phone's trust store.

## What to run

1. **Run full test** — permission, each camera, 2 switch cycles both ways,
   `applyConstraints` switch, overlapping open, 3 quick reopens. ~30–40 s.
2. Reproduce the Jitsi symptom: open Jitsi, switch front/back a few times, then
   come back to this page and press **Switch ×10** (it opens/stops alternately
   and logs every failure and timing).
3. **Snapshot** at any moment if something looks wrong (stripes, wrong colours,
   frozen image).

## Microphone

Press **Test microphone** (speak while it runs). It:

- lists `audioinput` devices before/after permission;
- reports the track settings (`sampleRate`, `channelCount`,
  `echoCancellation`, `noiseSuppression`, `autoGainControl`, `deviceId`);
- measures 1.5 s of `AudioContext` RMS/peak in dBFS and the silent ratio;
- records a 3 s clip, saved next to the report as `…-microphone.webm`;
- then requests audio **and** video together (the Jitsi/Meet path) and checks
  both tracks.

RooTitanium must be allowed to use the microphone: answer the in-app permission
dialog, and make sure **Settings › App Permissions › Microphone** is on. A
`NotAllowedError` in the report means one of those is off (or the site decision
was denied; reset it under Site Permissions).

At the end the page POSTs the report automatically (or press **Send**).

## Get the engine log too

The camera code logs on the native side. The launcher writes to
`/tmp/rootitanium.log` only when the marker file exists (it is a **file**, not an
environment prefix):

```sh
touch /home/rootitanium/DEBUG
# restart RooTitanium from the app grid, run the test, then:
grep -i 'droidmedia camera' /tmp/rootitanium.log
```

Useful lines:

- `N camera(s) found`
- `camera 0 (back): requested 1280x720 @ 30 fps`
- `preview 1280x720 ...` (what the vendor actually accepted)
- `first frame ... stride=... chroma_step=...` (one per session)
- `stopped (N frames, M dropped)` after each teardown
- `connect to the camera failed (busy?)`, `start_preview rejected`, vendor errors

A comparison run with the droidmedia path disabled is useful to attribute a
symptom:

```sh
RT_DROIDMEDIA_CAMERA=0 /usr/bin/harbour-rootitanium
```

## What to send back

- `tools/camera-test/reports/<timestamp>-<run>.json`
- the matching `*.jpg` snapshots
- `grep -i 'droidmedia camera' /tmp/rootitanium.log`

The JSON contains everything the page measured; the JPEGs are what let us judge
orientation, colour swap, stride stripes and chroma shift without guessing.
