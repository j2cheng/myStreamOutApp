# Architecture

This document describes the high-level architecture of the Stream Out APK project.

## Quick Start (Copy/Paste)

### Start Service

```bash
adb root
adb remount
adb shell am startservice com.crestron.txrxservice/.ServiceLauncher
```

### Open Control Console

```bash
telnet <device-ip-address> 9876
```

Press `Enter` twice for `TxRx>` prompt.

### Minimal StreamOut Session

```text
TxRx>mode=1
TxRx>TRANSPORTMODE=0
TxRx>VENCPROFILE=2
TxRx>RTSPPORT=1234
TxRx>VFRAMERATE=60
TxRx>VENCLEVEL=4096
TxRx>HDMIOUTPUTRES=1920x1080
TxRx>IPADDRESS=127.0.0.1
TxRx>start=true
TxRx>streamstate
TxRx>stop=true
```

## High-Level Diagram (ASCII)

```text
+----------------------------------------------+
| External Process / App                       |
| (sends control commands)                     |
+----------------------------+-----------------+
                             |
                             | gRPC control messages
                             v
+--------------------------------------------------------------------+
| streamOutApk (this project)                                        |
|                                                                    |
|  +-------------------------+                                       |
|  | Java Service Layer      |                                       |
|  | - StrmOutGrpcServer     |                                       |
|  | - StreamOutSvcCtrl      |                                       |
|  +------------+------------+                                       |
|               | JNI                                                |
|               v                                                    |
|  +-------------------------+    +-------------------------------+  |
|  | Native streaming layer  |    | Google Test (gtest)           |  |
|  | - streamout.cpp         |    | Tests native streaming layer  |  |
|  | - camera2_gst_streamer  |    |                               |  |
|  |   .cpp                  |    |                               |  |
|  | (in-project sources)    |    |                               |  |
|  +------------+------------+    +---------------+---------------+  |
|               |                                 |                  |
|               +---------------+-----------------+                  |
|                               |                                    |
|                               v                                    |
|                 +-------------------------------+                  |
|                 | GStreamer library (external)  |                  |
|                 | libgstreamer_android.so       |                  |
|                 +---------------+---------------+                  |
|                                 |                                  |
|                                 v                                  |
|                 +-------------------------------+                  |
|                 | GStreamer Pipeline            |                  |
|                 +-------------------------------+                  |
+--------------------------------------------------------------------+
```

## Component Notes

- All streaming logic is implemented by in-project native sources (`streamout.cpp`,
  `camera2_gst_streamer.cpp`, and their helpers); there is no longer a link to the
  external `cresStreamOut` (streamoutAPI) sources.
- The only external native dependency is the prebuilt GStreamer library
  (`libgstreamer_android.so` plus its headers), linked as an imported target.
- Google Test validates the in-project native streaming layer.
- gRPC receives control commands from external processes/apps.

## Data / Control Flow Summary

1. External process/app sends control commands over gRPC.
2. Java gRPC server and control classes process commands.
3. JNI/native C++ layer executes streaming operations.
4. Native layer drives the GStreamer pipeline directly via the GStreamer library.
5. Google Test exercises the same in-project native streaming layer to validate behavior.

## What's Working Today — VIDEO_AND_AUDIO_BOTH Path

This section focuses only on how video and audio are captured, encoded, and
streamed out as a single RTSP/RTP session.

### High-level diagram

```text
 ┌──────────────────────────── TSx80 (Android 12, QCS8250) ────────────────────────────┐
 │                                                                                       │
 │  USB-C  ┌─────────┐    Camera2     ┌────────────┐  Surface   ┌──────────────┐         │
 │  ◄────► │  UVC    │  ───────────►  │ CameraDev. │  ────────► │  MediaCodec  │         │
 │ (cam)   │  driver │                │ (id="1")   │            │ H.264/H.265  │         │
 │         └─────────┘                └────────────┘            │  HW encoder  │         │
 │                                                              └──────┬───────┘         │
 │                                                                     │ encoded NALs    │
 │                                                                     │ (JNI push)      │
 │                                                                     ▼                 │
 │                                                              ┌──────────────┐         │
 │                                                              │  GStreamer   │         │
 │  USB-C  ┌─────────┐  audio HAL    ┌────────────┐             │   appsrc     │         │
 │  ◄────► │  UAC    │  ───────────► │AudioFlinger│             │      ↓       │         │
 │ (audio) │ gadget  │  (USB_DEVICE  │  routes    │             │  h264parse?  │         │
 │         │ card 1  │   IN port)    │            │             │      ↓       │         │
 │         └─────────┘               └─────┬──────┘             │  rtph264pay  │         │
 │                                         │ PCM 44.1/mono      │   (pay0)     │         │
 │                                         ▼                    │              │         │
 │                                  ┌──────────────┐            │              │         │
 │                                  │ AudioRecord  │            │              │         │
 │                                  │  (CAMCORDER) │            │              │         │
 │                                  └──────┬───────┘            │              │         │
 │                                         │ PCM bytes          │              │         │
 │                                         ▼                    │              │         │
 │                                  ┌──────────────┐            │              │         │
 │                                  │  MediaCodec  │            │              │         │
 │                                  │  AAC encoder │            │   appsrc     │         │
 │                                  │ (audio/mp4a) │            │      ↓       │         │
 │                                  └──────┬───────┘            │   aacparse   │         │
 │                                         │ raw AAC frames     │      ↓       │         │
 │                                         │ (JNI push)         │  rtpmp4apay  │         │
 │                                         ▼                    │   (pay1)     │         │
 │                                                              └──────┬───────┘         │
 │                                                                     │                 │
 │                                                                     ▼ RTSP / RTP      │
 └─────────────────────────────────────────────────────────────────────┼───────────────┘
                                                                         │
                                                                rtsp://<ip>:8555/camera.sdp
```

### Audio capture path (Android audio stack)

```text
 AudioRecord(CAMCORDER)  ───►  AudioFlinger (in audioserver process)
                              │   (audioserver runs as user "audioserver",
                              │    group "audio" GID 1005, with sepolicy that
                              │    allows it to open /dev/snd/* nodes)
                              ▼
                          audio HAL  (vendor service)
                              │
                              ▼
                          LINE_DIGITAL input (id=24, type 6) — the laptop's
                                           digital audio over the USB-C / DisplayPort
                                           link, surfaced by the Lontium bridge.
                                           Native format 48 kHz stereo; AudioRecord
                                           requests 44.1 kHz mono (HAL resamples).
```

> **Audio source — verified June 2026.** The capture source is the on-box
> **`LINE_DIGITAL`** input (digital audio arriving over the USB-C / DisplayPort link
> via the Lontium bridge), selected by `AudioSource.CAMCORDER`. It is *not* a UAC
> USB-audio gadget — the "UAC gadget / card 1" labels in the diagrams above are a
> superseded guess. Source→device routing is fixed in the vendor audio-policy config
> and is **not** app-controllable; `setPreferredDevice()` and
> `AudioSource.UNPROCESSED` were both tested and do not override it. See
> `docs/AV_SYNC_NOTES.md` for the full topology and the clock / sync details.

## Program Flow Diagram

```text
+----------------------------+
| External App / Process     |
+-------------+--------------+
            |
            | gRPC cmd
            v
+----------------------------+
| StrmOutGrpcServer          |
+-------------+--------------+
            |
            v
+----------------------------+
| StreamOutSvcCtrl           |
+-------------+--------------+
            |
            | JNI
            v
+----------------------------+
| Native Bridge              |
+-------------+--------------+
            |
            v
+----------------------------+    +----------------------------+
| streamout.cpp /            |    | Google Test (gtest)        |
| camera2_gst_streamer.cpp   |    |                            |
+-------------+--------------+    +-------------+--------------+
              |                                 |
              | drives                          | validates
              v                                 v
              +-------------+-------------------+
                            |
                            v
                +----------------------------+
                | GStreamer library (ext)    |
                | libgstreamer_android.so    |
                +-------------+--------------+
                              |
                              v
                +----------------------------+
                | GStreamer Pipeline         |
                +----------------------------+
```

## Class Relationship Diagram

```text
Legend:
  ---> uses/calls
  o--> has-a/owns (contains and manages lifecycle)
  ^--- extends

+--------------------------------------------------------------+
| StreamOutSvcCtrl (Android Service entry point)               |
| - instance: StreamOutSvcCtrl (singleton)                     |
| - grpcServer: StrmOutGrpcServer                              |
| - port: int = 50051                                          |
| - grpcIp: String = "127.0.0.1"                              |
+--------------------------------------------------------------+
| + onCreate()                                                 |
| + onStartCommand(intent, flags, startId)                    |
| + getInstance()                                              |
| + nativeSetStreamoutPort(port)                              |
| + nativeSetStreamoutPipeline(pipeline)                      |
| + nativeStreamoutStart(arg)                                 |
| + nativeStreamoutStop(arg)                                  |
| + nativeStreamoutProjectInit(mode)                          |
| + jniRtspServerDebugWrapper(cmdString)                      |
+------------------------------+-------------------------------+
                               |
                               | o--> owns
                               v
+--------------------------------------------------------------+
| StrmOutGrpcServer                                             |
| - server: io.grpc.Server                                     |
+--------------------------------------------------------------+
| + start(ip, port)                                            |
| + stop()                                                     |
+------------------------------+-------------------------------+
                               |
                               | o--> owns service implementation
                               v
+--------------------------------------------------------------+
| StrmOutServiceImpl                                            |
+--------------------------------------------------------------+
| + sayHello(...)                                              |
| + rpcStreamoutSetPort(...)                                   |
| + rpcStreamoutSetStreamPipeline(...)                         |
| + rpcStreamoutRtspServerDebug(...)                           |
| + rpcStreamoutStart(...)                                     |
| + rpcStreamoutStop(...)                                      |
+------------------------------+-------------------------------+
                               |
                               | ^--- extends
                               v
+--------------------------------------------------------------+
| IntStreamoutSvcStrlGrpc.IntStreamoutSvcStrlImplBase          |
| (generated gRPC base class)                                  |
+--------------------------------------------------------------+

+----------------------------+       ---> uses       +----------------------------+
| StrmOutServiceImpl         | --------------------> | StreamOutSvcCtrl           |
| (handler methods)          |   getInstance()+JNI   | (native* methods)          |
+----------------------------+                       +----------------------------+

+--------------------------------------------------------------+
| Generated Protobuf / gRPC Types (grpc_str_out)               |
| - GrpcStrOut.SetPortRequest                                  |
| - GrpcStrOut.SetStreamPipelineRequest                        |
| - GrpcStrOut.StartRequest / StopRequest                      |
| - GrpcStrOut.RtspServerDebugRequest                          |
| - GrpcStrOut.StreamoutResponse                               |
+--------------------------------------------------------------+

+----------------------------+       ---> JNI calls   +---------------------------+
| StreamOutSvcCtrl           | ---------------------> | native-lib / streamout.cpp|
| (Java declarations)        |                        | (C++ implementation)      |
+----------------------------+                        +---------------------------+

Note:
    Android does not use a traditional main(); runtime entry is Service lifecycle
    methods (onCreate/onStartCommand) in StreamOutSvcCtrl.
```

## Known Issues / Open Items — A/V Sync & Timestamps

Running log of timestamp / PTS issues found during the audio-path review of
`nativePushAudioBuffer` (camera2_gst_streamer.cpp) and the audio thread in
`Camera2GstStreamer.java`. These are *correctness/clarity* notes — the current
code works on the always-on TSx device, but the reasoning below should be
captured before it is forgotten.

### Issue 1 — Audio vs video capture clock (RESOLVED: same clock, verified by measurement)

**How we found out (method):** added a read-only diagnostic in `listCameras()`
that reads `CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE` and logs it. On
the TSx (camera id=0, June 2026) logcat printed:

```text
id=0 SENSOR_INFO_TIMESTAMP_SOURCE=1 (REALTIME / BOOTTIME / elapsedRealtimeNanos)
```

`SENSOR_INFO_TIMESTAMP_SOURCE = 1 = REALTIME` means the Camera2 sensor stamps
every buffer with `CLOCK_BOOTTIME` (the `elapsedRealtimeNanos()` base). The
audio path already uses `elapsedRealtimeNanos()`. **Therefore audio and video
are on the SAME clock — there is no cross-clock mismatch.** The earlier concern
in this issue (BOOTTIME audio vs MONOTONIC video) was based on an *assumption*
about the sensor clock that the measurement disproved.

| Source | API used | Clock domain (measured) |
|---|---|---|
| Video `presentationTimeUs` (Camera2 sensor timestamp) | MediaCodec output `info.presentationTimeUs` | **`CLOCK_BOOTTIME`** (`SENSOR_INFO_TIMESTAMP_SOURCE=1`) |
| `s_pts_base` (first video frame, the normalization anchor) | derived from the video PTS above | **`CLOCK_BOOTTIME`** |
| Audio `ptsUs` (capture-start time) | `android.os.SystemClock.elapsedRealtimeNanos()` | **`CLOCK_BOOTTIME`** |
| GStreamer pipeline clock (`on_media_configure`) | `gst_system_clock_obtain()` | `CLOCK_MONOTONIC` (does not matter — PTSs are normalised to `s_pts_base`) |

**What the offset actually is (now that the clock question is settled):**

Both PTSs are honest `CLOCK_BOOTTIME` capture-times, and the shared `s_pts_base`
cancels in their difference, giving a clean same-clock comparison:

```text
raw_audio_pts       = audio_capture_boottime − s_pts_base
s_last_video_pts_ns = video_capture_boottime − s_pts_base

instant_offset = raw_audio_pts − s_last_video_pts_ns
               = audio_capture_boottime − video_capture_boottime   ← base cancels
```

**Measured (logcat.txt, 2026-06-17, full ~65 s session):** `instant_offset` is
**positive**, ≈ +66..+110 ms (mean ≈ +85 ms) — audio capture-time *ahead* of
video capture-time, every frame. (An earlier run reported a negative offset;
that does **not** reproduce on this build.) Note: the periodic `A/V_offset`
value in logcat is **not** `instant_offset` — it is the *post-correction
residual* (`corrected − video`), which the EMA holds near 0, ≈ ±20 ms.

**Do NOT switch audio to `System.nanoTime()`:**
- `System.nanoTime()` = `CLOCK_MONOTONIC`; `elapsedRealtimeNanos()` =
  `CLOCK_BOOTTIME`. Since the camera is BOOTTIME (`SENSOR_INFO_TIMESTAMP_SOURCE=1`),
  the **current** `elapsedRealtimeNanos()` audio timestamp is the one that
  matches video. Switching to `System.nanoTime()` would *introduce* a
  `BOOTTIME−MONOTONIC` skew (= accumulated suspend; ≈0 on always-on TSx, but wrong
  in principle).
- This **reverses** the earlier recommendation in this document, which assumed
  the camera was MONOTONIC. Keep `Camera2GstStreamer.java` line ~1688 on
  `elapsedRealtimeNanos()`.
- Conditional future note: **if** a different product reports
  `SENSOR_INFO_TIMESTAMP_SOURCE = 0` (UNKNOWN / MONOTONIC), then on *that* device
  the audio capture timestamp should switch to `System.nanoTime()` to match.
  Decide per-device from the logged timestamp source, not by assumption.

**EMA status:** the instant offset is positive every frame, so the `[0, 500 ms]`
clamp does **not** zero it — `s_av_offset_ema` converges to and tracks ≈ +85 ms
(observed 71..98 ms) and `corrected_pts = raw_audio_pts − s_av_offset_ema` shifts
the audio PTS back onto the video timeline. The EMA correction is therefore
**active and effective**; the post-correction residual oscillates ≈ ±20 ms around
0 (near the ≤10 ms target). Without it the audio would reach the client ~85 ms
ahead of video.

**Stale doc-comment still to verify:** the `nativePushAudioBuffer` header comment
should describe that it SETS `GST_BUFFER_PTS = corrected_pts` once `s_pts_base`
is established (the do-timestamp fallback only applies to the early frames before
the first video frame arrives).

### Issue 2 — The audio-ahead offset and the (active) EMA correction

With audio and video confirmed on the same `CLOCK_BOOTTIME` capture clock, the
measured `instant_offset = audio_capture_boottime − video_capture_boottime` is
**positive (≈ +66..+110 ms, mean ≈ +85 ms)** — the audio PTS runs *ahead* of the
video PTS, every frame (logcat.txt 2026-06-17).

The EMA tracks this ~+85 ms offset and subtracts it, so `corrected_pts` lands on
the video timeline and the post-correction residual stays ≈ ±20 ms around 0. The
correction is **active**, not inert.

*Open items to discuss next:*
- *Verify on the real client that the corrected stream is lip-synced (the ≈ ±20
  ms residual should be inaudible/invisible) and the ~+85 ms shift is right.*
- *Root-cause WHY the audio PTS is ~85 ms ahead of video — e.g. the camera sensor
  timestamp is the exposure/SOF time while the audio capture-start back-dating
  under-counts HAL buffering — so the EMA target is understood, not just empirical.*
- *Confirm the earlier negative reading was a different condition (cold start,
  different source/rate) and not an intermittent regime we must also handle.*

### Issue 3 — Audio capture latency budget (glass-to-glass)

Independently of the A/V *offset* (which the EMA corrects — Issue 2), the audio
path still spends real wall-clock time in HAL/encoder buffers before its AAC
frame exists to be pushed. This section budgets that absolute capture latency —
it affects glass-to-glass delay and the client jitter buffer, **not** lip-sync
(the EMA already aligns the PTS). The PTS itself is honest — back-dated to
capture-start at `Camera2GstStreamer.java` line ~1707 — so the only way to shrink
this latency is to make the content physically move through the pipeline faster;
relabelling the PTS cannot help.

Where the ~100 ms accumulates in the audio path:

| Stage | Where in code | Approx. latency | Controllable? |
|-------|---------------|-----------------|---------------|
| 1. AudioRecord HAL capture | `AudioSource.CAMCORDER`, `recBufSize = max(minBuf*4, 8192)` (line ~1610–1617) | 20–50 ms | **Yes** |
| 2. PCM read chunking | reads 20 ms at a time (`pcmFrameBytes`, line ~1663) | up to 20 ms | **Yes** |
| 3. AAC framing | encoder must fill 1024 samples = 23.2 ms before it can emit a frame | ~23 ms | No (intrinsic to AAC-LC) |
| 4. HW AAC encoder pipeline depth | `OMX.qcom.audio.encoder.aac` (line ~286) | 23–46 ms (1–2 frames) | Partially |

By contrast the **video** path is essentially stage-1-only (sensor → HW H.264
encoder, ~1–2 frames ≈ 33–66 ms). (Note: despite this audio-side buffering, the
measured *PTS* offset has audio ~85 ms *ahead* of video — see Issue 2 — because
the back-dated capture-start timestamp under-counts the HAL buffering above; the
EMA absorbs the net difference.)

**Levers, in order of payoff/risk:**

1. **`recBufSize` (line ~1610) — easiest, lowest risk.** The `×4` multiplier and
   the 8192-byte floor (≈93 ms of mono 44.1 kHz audio) let the HAL hold a large
   standing backlog. Try `minBuf * 2`. Too small risks `read()` overruns/dropouts,
   so `*2` is the safe experiment, not `*1`.
2. **AudioRecord source + fast-capture path (line ~1612).** `CAMCORDER` is tuned
   for A/V capture, not low latency. Switching to `AudioSource.UNPROCESSED` skips
   AGC/NS stages. The Android fast/low-latency capture path only engages at the
   device-native rate (almost certainly **48000** on QCS8250, not 44100) and
   native frames-per-buffer — so 44100 currently forces a resampler *and*
   disqualifies the fast path. Moving the whole audio chain to 48 kHz is the
   biggest structural win but also touches the GStreamer caps/`codec_data`.
3. **Read chunk size (line ~1663).** Halving 20 ms → 10 ms shaves ~10 ms at the
   cost of 2× wakeups. Marginal.
4. **HW encoder pipeline (stages 3–4) — hidden big one.** Mostly not exposed by
   public API. Try `fmt.setInteger(MediaFormat.KEY_LATENCY, 1)` in the encoder
   config (line ~1651); some Qualcomm encoders honour it. Also A/B-test the
   **software** AAC encoder (`createEncoderByType` path, line ~1643): it often has
   a shallower pipeline than the HW one — lower latency at the cost of CPU.

**Caveat:** reducing audio capture latency does **not** improve lip-sync (the EMA
already aligns the PTS). It only reduces glass-to-glass latency and eases the
client jitter buffer. Pursue this only if there is an end-to-end latency target
or evidence the client is dropping late audio; otherwise "do nothing" remains
correct.

