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
choice; the container itself sits behind NAT at `10.x.x.x`):

```sh
cd tools/camera-test
python3 server.py
```

It prints something like:

```
  camera test:  https://<server-ip>:8443/
  reports:      /.../tools/camera-test/reports
```

On the phone open that URL, accept the self-signed certificate warning once.
`getUserMedia` requires a secure context; plain HTTP will not work (except on
`localhost`, which the phone is not).

You can also copy `index.html` to any HTTPS host you already have. Then use
**Download JSON** instead of **Send to server** (the page has no other network
access).

## What to run

1. **Run full test** — permission, each camera, 2 switch cycles both ways,
   `applyConstraints` switch, overlapping open, 3 quick reopens. ~30–40 s.
2. Reproduce the Jitsi symptom: open Jitsi, switch front/back a few times, then
   come back to this page and press **Switch ×10** (it opens/stops alternately
   and logs every failure and timing).
3. **Snapshot** at any moment if something looks wrong (stripes, wrong colours,
   frozen image).

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
