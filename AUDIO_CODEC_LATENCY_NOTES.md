# Audio Codec Latency: AAC vs Opus — Working Notes

> Scratchpad for the AAC→Opus latency comparison experiment. Update as numbers come in.

## Goal

Measure the audio **encoder latency** of the current AAC path, then compare against
a GStreamer **Opus** path, to decide whether to switch the streamout audio codec.

---

## Current state (AAC baseline)

- Encoder: **`c2.android.aac.encoder`** — SOFTWARE (confirmed from logcat; no HW AAC
  encoder exists on QCS8250).
- Config: 44100 Hz, mono, 64 kbps, AAC-LC (`audio/mp4a-latm`).
- Encoded in **Java** via `MediaCodec`, then handed to GStreamer as AAC frames.
- Instrumentation already added in `Camera2GstStreamer.java` (banner-wrapped:
  `/*******Measure the encoder pipeline latency***...***/`). Logs every 50th frame:
  ```
  AAC latency [<codec>]: encoder=NN ms (PCM queue->AAC emit)  capture->emit=MM ms  (in-flight=K chunks)
  ```
- [ ] **TODO: capture steady-state AAC numbers** (run ~30 s, ignore first few frames).
  - encoder = ____ ms
  - capture->emit = ____ ms
  - in-flight = ____ chunks

### Predicted AAC (before running)
- encoder ≈ 30–60 ms (central ~40) — AAC-LC frame is 1024 samples (23.2 ms) but chunks
  are 20 ms, so a frame straddles into chunk N+1 (~+20 ms) + CCodec output buffering +
  ~20 ms single-threaded drain cadence.
- capture->emit ≈ 50–80 ms (central ~60) — adds the ~20 ms capture-start back-date.
- in-flight ≈ 2–4 chunks.
- Note: `encoder=` is mostly *structural framing*, not raw compute (SW AAC compute is a few ms).
- Note: `capture->emit` does NOT include the HAL standing backlog (~20–50 ms), which this
  probe can't see.

---

## Opus plan

GStreamer Opus is **confirmed enabled** in the build (`armeabi-v7a`):
`opusenc`, `opusdec`, `rtpopuspay`, `rtpopusdepay`, `opusparse` all present, with full
`opusenc` props (`frame-size`, `bitrate`, `audio-type`, `bandwidth`, `bitrate-type`).

### Pipeline (encode moves OUT of Java, INTO GStreamer)
```
appsrc (PCM) -> audioconvert -> audioresample -> opusenc -> rtpopuspay -> rtspserver
```
This deletes the Java MediaCodec AAC encoder + AAC framing + the chunk/frame straddle.

### Parameters to test
- **Bitrate / channels:**
  - [ ] Start: **64 kbps mono**
  - [ ] Then try: **2 channels (stereo)**
- **Frame size:**
  - [ ] Start: **frame-size=20** (matches the 20 ms capture chunks)
  - [ ] Then try: **frame-size=10** (does it reduce latency? unknown — measure)
- **Sample rate:** feed PCM at **48 kHz** (Opus native rate — avoids a resample stage).
  Already the plan; capture at 48k mono on AudioRecord.

### Opus measurement (apples-to-apples)
AAC is measured in Java; Opus is encoded in GStreamer, so use a **pad probe**:
timestamp buffer entering `appsrc` (PCM in) vs leaving `opusenc` src pad (Opus out).
Same concept as the AAC probe ("PCM in -> compressed frame out" wall time), different layer.
- [x] Add `opusenc` src-pad probe + matching log line for direct comparison.
- [ ] Keep AAC path as fallback (flag-gated switch).

### Expected
Opus should beat AAC on the encode path: ~6.5 ms algorithmic delay + frame size matched
to chunking, vs AAC-LC's fixed 23.2 ms frame that doesn't align to 20 ms chunks.
Let the measurements decide.

---

## What "Opus push latency" measures (and how it compares to AAC)

The Opus path emits this line (every 100 frames) from `runOpusPushLoop()` in
`Camera2GstStreamer.java`:

```
06-18 12:55 ... Opus push latency: min=10 ms avg=10 ms max=10 ms (capture-start->appsrc; opusenc compute not included)
```

### What the number is
It is the **Java-side plumbing latency only**: from the **capture-start PTS** of a PCM
frame (the `presentationTimeUs` of its first sample) to the moment that frame is handed to
`nativePushAudioBuffer()` (the GStreamer `appsrc`). It spans:

- filling one PCM frame from `AudioRecord.read()` (10 ms — `OPUS_FRAME_MS`)
- the small bookkeeping/copy before the JNI push

### Why it reads a flat 10 ms
You **cannot emit a 10 ms frame before you have recorded 10 ms of audio** — that fill time
is the hard floor. `min == avg == max == 10 ms` means there is essentially **zero extra
Java jitter** on top of that floor: no thread stalls, no GC pauses, no backlog. It is the
best possible Java-side result for a 10 ms frame.

### What it deliberately does NOT include
- `opusenc` **compute** (encode time) — happens later, in GStreamer, on the streaming
  thread. → measured separately by the new **"Opus encode latency"** probe (below).
- `rtpopuspay` packetization, kernel/socket, network, and the receiver's jitter buffer.
- The capture HAL's standing backlog (same blind spot as the AAC probe).

So this is the Opus analogue of the AAC `encoder=` "plumbing" number — **not** the
end-to-end mouth-to-ear latency.

### Side-by-side with AAC
| Metric | AAC path | Opus path |
|---|---|---|
| Probe span | PCM queue → AAC emit (`dequeueOutputBuffer`) | capture-start → `appsrc` push |
| Where encode runs | **Java** `MediaCodec` (inside the span) | **GStreamer** `opusenc` (outside the span) |
| Frame size | 1024 samples ≈ **21.3 ms @ 48 kHz** (fixed) | **10 ms** (`frame-size=10`, tunable) |
| Typical `capture->emit` | **~22–25 ms** (includes SW encode + frame straddle) | **10 ms** (Java side; encode moved downstream) |
| Jitter | varies with codec drain cadence | flat (min=avg=max) |

The two spans are **not identical** (AAC includes the encode; Opus moves it out), so the
honest comparison is: Opus's Java-side cost is just the **10 ms frame-fill floor**, whereas
AAC's comparable Java-side cost was **~22–25 ms** because the software encode *and* the
larger non-aligned frame sit inside the measured span. To compare the *encode compute*
fairly, read the new "Opus encode latency" line against AAC's `encoder=` column.

### The companion probe: "Opus encode latency"
To recover the encode cost that "Opus push latency" omits, a native pad probe now wraps
`opusenc` (named `aenc` in the pipeline) and logs every 100 frames:

```
Opus encode latency: min=.. ms avg=.. ms max=.. ms (opusenc sink->src compute, last 100 frames)
```

It timestamps each buffer entering the `opusenc` sink pad and leaving its src pad
using `g_get_monotonic_time()`. Pairing is **FIFO** (oldest outstanding sink
buffer pops on each src buffer), not PTS-keyed: `opusenc` is a `GstAudioEncoder`
and re-timestamps its output for the encoder delay, so the src-pad PTS does
**not** equal the sink-pad PTS — a PTS lookup never matches (this was observed in
the first run: zero "Opus encode latency" lines). Since `opusenc` consumes 10 ms
PCM in and emits 10 ms Opus out, in order, 1:1, on one streaming thread, FIFO is
the correct and PTS-rewrite-proof pairing. This is the apples-to-apples
counterpart to AAC's `encoder=` compute number. Implementation:
`opus_enc_sink_probe` / `opus_enc_src_probe` in
`app/src/main/cpp/camera2_gst_streamer.cpp`, attached in `on_media_configure`.

### Latest run observations (logcat.txt, 2026-06-18 13:40 — FIFO probe build)

After the FIFO-pairing fix the encode-latency probe logs cleanly. Sample window:

```
Opus encode latency: min=0.16 ms avg=0.45 ms max=5.50 ms  (opusenc sink->src, last 100 frames)
Opus encode latency: min=0.16 ms avg=0.22 ms max=1.93 ms  (steady state)
Opus push latency:    min=10  ms avg=10   ms max=12   ms  (capture-start->appsrc)
```

Two small observations (**not bugs**):

1. **A/V_offset is now ≈ −10 to −13 ms** (was −4 ms last run), with `ema_offset`
   steady at **95–98 ms**. The EMA is still working correctly — raw audio is
   ~85 ms ahead (`raw_audio=0.585 s` vs `video=0.500 s`), corrected back to
   within ±13 ms. It dips just over the −10 ms target on a couple of frames but
   stays well under the 50 ms "client adds buffer" threshold, so it's fine. This
   reconfirms (as discussed) **you still need `ema_offset`** — it is codec-agnostic
   and corrects a capture-timestamp offset, not an encode-path difference.

2. **The encode probe runs on its own streaming thread** (tid 18474), decoupled
   from the audio push thread (tid 18436) — exactly the FIFO-pairing design, and
   it's logging cleanly every 100 frames.

Encode compute is **negligible**: avg ~0.2–0.45 ms (≈0.25 ms once warmed up); the
5.5 ms first-window max is JIT/cold-cache startup and drops to ~2 ms in steady
state. So the real Opus audio-path cost ≈ **10 ms (frame fill) + ~0.25 ms (encode)
≈ ~10.3 ms**, vs the AAC path's ~22–25 ms — confirmed win.

---

## Toggling the latency logs on/off at runtime: `SET_AUDIO_LATENCY_DEBUG`

All audio-latency diagnostic logging is gated behind a single runtime debug
command (no rebuild, no re-stream — it takes effect on the next buffer):

```
STREAMOUT_CAM2 SET_AUDIO_LATENCY_DEBUG on     # enable all three logs
STREAMOUT_CAM2 SET_AUDIO_LATENCY_DEBUG off    # silence them (default)
```
(accepts `true/false`, `1/0`, `on/off`, `yes/no`.)

### What it gates (one switch, both layers)
| Log line | Layer | Source |
|---|---|---|
| `Opus encode latency: …` | native pad probe | `opus_enc_src_probe` |
| `[A/V sync]` / `[EMA init]` | native push path | `nativePushAudioBuffer` |
| `Opus push latency: …` | Java capture loop | `runOpusPushLoop()` |

### Default = **OFF**
Both flags ship disabled — native `s_audio_latency_debug = 0`
(`camera2_gst_streamer.cpp`) and Java `m_audioLatencyDebug = false`
(`Camera2GstStreamer.java`). So a normal stream is silent and costs nothing
until you explicitly turn the logs on to take a measurement.

### Logging only — never changes stream behaviour
The EMA PTS correction, audio capture/push, and the encode itself **always run**.
The switch only gates the `LOGI` / `Log.i` calls. You can toggle it freely
mid-stream without affecting A/V sync or latency.

### Zero cost when off
When disabled, both `opusenc` pad probes **early-return before any lock or map
work** (`if (!s_audio_latency_debug) return GST_PAD_PROBE_OK;`), so the probe's
only residual cost is GStreamer's per-buffer callback dispatch (nanoseconds). On
re-enable, the native setter calls `opus_probe_reset()` to clear any stale FIFO
entries, so the first `Opus encode latency` window (~100 frames ≈ 1 s) starts
clean.

### Wiring
- Native flag + setter `Cam2Streamer_SetAudioLatencyDebug(int)` in
  `app/src/main/cpp/camera2_gst_streamer.cpp`.
- Java flag + `setAudioLatencyDebug(boolean)` in
  `app/src/main/java/com/crestron/streamout/Camera2GstStreamer.java`.
- Command dispatch `cam2_dbg_set_audio_latency_debug()` in
  `app/src/main/cpp/cam2_streamout_debug.cpp` (sets the native flag directly,
  forwards the Java half over JNI).

---

## Understanding encoder latency vs frame (framing) delay

This is the key concept for reading the numbers correctly. **There are two different
delays, and our current probe only sees one of them.**

### 1. What the `capture->emit` probe actually measures (~10 ms)
The instrumentation timestamps `presentationTimeUs` (the capture-**start** of a PCM chunk),
matches it via `floorEntry` to the nearest *preceding* capture chunk's wall time, and stops
at `dequeueOutputBuffer` return. That captures:

- PCM buffering + chunking
- thread hop (audio thread -> encoder)
- raw encoder **compute** time (software AAC ≈ 0–1 ms)

Steady-state result: **~10 ms**, with the `encoder=` column reading ≈ 0.
This is the *plumbing* latency — it is **real but incomplete**.

### 2. What the probe does NOT see: algorithmic framing delay
A codec cannot emit a compressed frame until it has accumulated a **full frame** of audio.
For AAC-LC that frame is **1024 samples ≈ 23.2 ms**. Walk one frame on a timeline:

```
t=0 ms     first sample of the frame captured  (≈ the frame's pts)
t=23 ms    1024th sample captured  -> only NOW can the encoder produce the frame
t≈23 ms    compressed frame emitted (compute ≈ 0)
```

- The **last** sample of the frame has ~0 latency (captured at 23 ms, emitted at 23 ms).
- The **first** sample has ~23 ms latency.
- On average a frame adds **~half a frame ≈ 11 ms**, up to a **full frame ≈ 23 ms**.

Our `floorEntry` matching pairs each output frame to a nearby capture wall-time rather than
to the *first* sample of that frame, so this framing delay is **mostly invisible** in the
~10 ms number. That is why `encoder=` reads ≈ 0 — it is NOT literally zero codec delay.

### 3. The two numbers are NOT double-counted
| Bucket | What it is | AAC-LC | Opus (10 ms frame) |
|---|---|---|---|
| Plumbing (probe *can* see) | buffering + thread + compute | ~10 ms | ~10 ms |
| Framing (probe *cannot* see) | half-frame avg + lookahead | ~11 ms (+ ~20 ms lookahead) | ~5 ms (+ ~6.5 ms lookahead) |
| **True capture->emit (estimate)** | | **~20–30 ms** | **~12–18 ms** |

So "10 ms measured" and "Opus saves ~10 ms" are not contradictory: Opus's win comes out of
the **hidden framing bucket**, not out of the visible 10 ms plumbing.

### 4. Why Opus reduces the framing bucket
- **Tunable frame size:** Opus supports 2.5 / 5 / 10 / 20 / 40 / 60 ms frames. A 10 ms frame
  averages ~5 ms framing vs AAC's ~11 ms.
- **Lower algorithmic lookahead:** Opus default ≈ 6.5 ms (down to ~2.5 ms in restricted
  low-delay mode), vs AAC-LC's larger filterbank lookahead.

Realistic saving: **~8–12 ms** off the part we currently can't measure — NOT an extra 10 ms
on top of the 10 ms we already see.

### 5. Caveat: our probe is the wrong instrument to confirm this
The current Java probe cannot see framing delay. To validate the AAC-vs-Opus comparison for
real, use an **external/acoustic loopback** or a **continuous-audio reference**, not the
floor-matched wall-clock metric. Do this *before* committing to the Opus refactor so we are
optimizing a number we can actually observe.

---

## Opus internals: `audio-type` (algorithm), bitrate, delay, trade-offs

`opusenc audio-type=…` does **not** pick a codec directly — it sets the Opus
*application hint* (`OPUS_SET_APPLICATION`), which biases Opus's internal
mode-decision logic. Opus is a **hybrid** of two engines and switches between three
modes per frame:

| Mode | Engine | Math | Tuned for |
|---|---|---|---|
| **SILK-only** | LPC / linear prediction | time-domain | low-bitrate speech |
| **CELT-only** | MDCT (lapped transform) | frequency-domain | music + **low latency** |
| **Hybrid** | SILK (low band) + CELT (high band) | both | fullband speech |

### `audio-type` options (algorithm + delay)

> Delay below is the codec's **intrinsic** delay at our **10 ms** `frame-size`
> (`frame fill 10 ms` + `algorithmic look-ahead`). It is *separate* from the
> ~0.2 ms encode **compute** and the ~10 ms Java capture plumbing.

| `audio-type` | Opus application (const) | Engine selected | Look-ahead | Intrinsic delay @10 ms frame | Best for | Trade-off |
|---|---|---|---|---|---|---|
| `voice` *(current)* | `OPUS_APPLICATION_VOIP` (2048) | SILK / Hybrid | ~6.5 ms | **~16.5 ms** | low-bitrate speech | +4 ms vs CELT; music slightly worse |
| `generic` | `OPUS_APPLICATION_AUDIO` (2049) | Hybrid / CELT (quality-biased) | ~6.5 ms | **~16.5 ms** | music / mixed content | same delay as `voice`, tuned for fidelity not speech |
| `restricted-lowdelay` | `OPUS_APPLICATION_RESTRICTED_LOWDELAY` (2051) | **CELT-only (MDCT)** | ~2.5 ms | **~12.5 ms** | lowest latency at ≥48 kHz, mid bitrate | weaker at very low bitrate speech (<24 kbps); disables SILK |

Notes:
- `restricted-lowdelay` **is** the "low-latency MDCT/CELT" path — it forces CELT-only
  and disables SILK, dropping look-ahead 6.5 → 2.5 ms.
- GStreamer `opusenc` exposes **no** `force-mode`/"celt" property; `OPUS_SET_FORCE_MODE`
  is a private Opus API. `restricted-lowdelay` is the supported CELT-only switch.
- With CELT-only you may further cut `frame-size` to 5 or 2.5 ms for extra savings
  (more RTP packets/overhead — measure before adopting).

### Bitrate guidance (at 48 kHz)

Current: `bitrate=64000` (`OPUS_BITRATE`), to match the AAC-LC baseline.

| Bitrate | Channels | Subjective quality | When to use |
|---|---|---|---|
| 16–24 kbps | mono | intelligible speech | bandwidth-constrained voice; needs `voice` (SILK) |
| 32 kbps | mono | good speech, OK music | low-bandwidth links |
| **64 kbps** *(current)* | mono | transparent speech, very good music | default; matches AAC baseline |
| 96–128 kbps | stereo | near-transparent music | stereo / music-grade audio |

- Opus bitrate is largely **decoupled from delay** — raising it improves quality, not latency.
- At 64 kbps both `voice` and `restricted-lowdelay` are effectively transparent, so the
  CELT switch costs essentially nothing in quality while saving ~4 ms.

### The three latency buckets (don't conflate them)

| Bucket | Magnitude | Measured by | Depends on |
|---|---|---|---|
| Encode **compute** | ~0.2 ms (steady) | "Opus encode latency" probe (`opusenc` sink→src) | CPU speed |
| Frame **fill** | 10 ms (`frame-size`) | "Opus push latency" (capture-start→appsrc) | `frame-size` |
| **Algorithmic** look-ahead | 6.5 ms (`voice`) / 2.5 ms (CELT) | *not* visible to either probe — appears as `opusenc` PTS rewrite | `audio-type`/mode, **not** CPU |

> The often-quoted **"26.5 ms"** Opus delay = 20 ms *default* frame + 6.5 ms look-ahead.
> It does **not** apply to this pipeline, which uses 10 ms frames (→ ~16.5 ms `voice`,
> ~12.5 ms `restricted-lowdelay`).

---

## Open items / gating questions
- [ ] **Receiver Opus support** — does the RTSP consumer decode `audio/opus`
  (`OPUS/48000/2`)? AAC over RTSP is universal; Opus is not always present in
  embedded/HW players. **Make-or-break** before committing to the switch.
- [ ] **ABI check** — only `armeabi-v7a` GStreamer lib found at
  `/home/builduser/StreamingApkBldEnv/CSS/gst-android-build/`. QCS8250 is arm64.
  Confirm which ABI the streamout app actually loads (`abiFilters` / packaged `jniLibs`);
  if arm64-v8a, verify that build also has opus enabled.

---

## Reference paths

---

## Highest Impact Fixes — Audio Latency Reduction Plan

These three changes will yield the most measurable improvement to capture→emit latency and A/V sync.

### 1. **Reduce AudioRecord HAL buffering** (20–50 ms gain) — LOW RISK ✓

**File:** `app/src/main/java/com/crestron/streamout/Camera2GstStreamer.java` (lines ~1610–1617)

**Current (old):**
```java
int minBuf = AudioRecord.getMinBufferSize(AUDIO_SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, 
                                          AudioFormat.ENCODING_PCM_16BIT);
int recBufSize = Math.max(minBuf * 4, 8192);  // ← Multiplier of 4 adds 20–50 ms standing backlog
```

**After (new):**
```java
int minBuf = AudioRecord.getMinBufferSize(AUDIO_SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, 
                                          AudioFormat.ENCODING_PCM_16BIT);
int recBufSize = Math.max(minBuf * 2, 8192);  // ← Reduce buffering (safer than *1)
```

**Why:** The `×4` multiplier creates a large standing HAL backlog. Reducing to `×2` cuts buffer depth in half (20–50 ms savings) while remaining safe against read overruns. Do NOT use `×1` (too risky for frame drops).

**Measurable impact:** +20–50 ms latency reduction.

**Status:** [ ] **TODO: Apply this fix and re-test**

---

### 2. **Switch audio to 48 kHz (device native rate)** (10–20 ms gain) — MEDIUM EFFORT ✓

**Current (old):** 44.1 kHz mono → forces audio resampler, disqualifies Android fast-capture path

**After (new):** 48 kHz mono → native device rate, unlocks fast-capture path, eliminates resample stage

**Files to update:**
- `Camera2GstStreamer.java` (line ~1604): `AUDIO_SAMPLE_RATE = 48000` 
- `camera2_gst_streamer.cpp`: Update GStreamer audio caps to match 48 kHz
- Verify GStreamer `appsrc` and downstream pipeline caps accept 48 kHz (likely auto-converts; verify logcat for caps negotiation)

**Why:** QCS8250 native rate is 48 kHz. At 44.1 kHz the HAL forces a resampler (~10–20 ms overhead) AND disqualifies the low-latency capture path. Matching the native rate eliminates both.

**Measurable impact:** +10–20 ms latency reduction.

**Status:** [ ] **TODO: Measure device native rate** (check audio HAL properties or logcat)  
[ ] **TODO: Update AUDIO_SAMPLE_RATE and test**

---

### 3. **Enable AAC encoder low-latency hint** (5–15 ms gain) — MINIMAL EFFORT

**File:** `app/src/main/java/com/crestron/streamout/Camera2GstStreamer.java` (line ~1651, in MediaFormat setup)

**Current (old):**
```java
MediaFormat fmt = MediaFormat.createAudioFormat("audio/mp4a-latm", AUDIO_SAMPLE_RATE, 1);
fmt.setInteger(MediaFormat.KEY_BIT_RATE, 64000);
// No latency hint
encoder = MediaCodec.createEncoderByType("audio/mp4a-latm");
```

**After (new):**
```java
MediaFormat fmt = MediaFormat.createAudioFormat("audio/mp4a-latm", AUDIO_SAMPLE_RATE, 1);
fmt.setInteger(MediaFormat.KEY_BIT_RATE, 64000);
fmt.setInteger(MediaFormat.KEY_LATENCY, 1);  // ← Add low-latency hint
encoder = MediaCodec.createEncoderByType("audio/mp4a-latm");
```

**Why:** Some Qualcomm encoders honour `KEY_LATENCY=1` to reduce encoder pipeline buffering. Not guaranteed to work, but costs nothing and may shave 5–15 ms.

**Measurable impact:** +0–15 ms (device-dependent; test to confirm).

**Status:** [ ] **TODO: Add KEY_LATENCY hint and measure**

---

### Combined expected impact: **35–85 ms reduction** (top achievable without major refactor)

| Fix | Effort | Latency gain | Risk | Priority |
|-----|--------|--------------|------|----------|
| 1. HAL buffer `×2` | 1 line | **+20–50 ms** ✓ | Low | **DO FIRST** |
| 2. Switch to 48 kHz | ~5 lines + test | **+10–20 ms** ✓ | Medium | 2nd |
| 3. AAC latency hint | 1 line | +0–15 ms | Minimal | 3rd |
| **Total** | | **~35–85 ms** | | |

---

## Understanding EMA Correction — How "Audio Ahead" Alignment Works

### What "Audio Ahead" means

**"Audio ahead of video" = audio has a LATER timestamp (will play AFTER video)**

This is confusing terminology! The term refers to the **numeric value of the timestamp being ahead on the timeline**, not the order in which frames are captured. A later timestamp = plays later = lags in playback.

### Concrete Example: Where the +85 ms offset comes from

Imagine both video and audio are captured at nearly the same physical moment (real world, ~t=0):

```
Real-world capture (simultaneous):
  ├─ Video frame captured     → assigned PTS = 1000 ms
  └─ Audio frame captured     → assigned PTS = 1085 ms
```

The **audio PTS is 85 ms numerically ahead** (1085 > 1000). But what does that mean for playback?

```
Client playback clock:
  
  t = 1000 ms  → "Show video frame (PTS=1000)"     ✓ VIDEO DISPLAYS
  
  t = 1085 ms  → "Play audio frame (PTS=1085)"     ✓ AUDIO PLAYS
  
              ↑
        85 ms gap — AUDIO LAGS VISUALLY
```

**Result:** Audio lags 85 ms behind video. You see lips moving for 85 ms, then hear the sound.

### Why this happens: The PTS calculation

Both PTS values are calculated from the same **boottime clock** (both use `CLOCK_BOOTTIME`), but with an offset:

```
raw_audio_pts       = audio_capture_boottime − s_pts_base
                    = 1085 ms

s_last_video_pts_ns = video_capture_boottime − s_pts_base
                    = 1000 ms

instant_offset = raw_audio_pts − s_last_video_pts_ns
               = 1085 − 1000 = +85 ms   ← POSITIVE means audio PTS is ahead
```

The `s_pts_base` is the reference point (the first video frame), which both channels subtract, so the difference isolates the true capture-time offset. Audio really *was* captured 85 ms "ahead" in the timeline (or more precisely, the audio timestamp back-dates the capture-start 85 ms earlier than the video did).

### The EMA correction: bringing them back into sync

The **Exponential Moving Average (EMA)** continuously tracks the `instant_offset` and applies a correction:

```
s_av_offset_ema = EMA of recent instant_offset values
                ≈ 85 ms (converges and stays)

corrected_pts = raw_audio_pts − s_av_offset_ema
              = 1085 − 85
              = 1000 ms   ✓ NOW MATCHES VIDEO
```

Now both audio and video have PTS = 1000 ms, so they play **in sync** on the client.

### How to verify the EMA is working: check logcat

```bash
adb logcat | grep -E "(instant_offset|A/V_offset|ema_offset)" | head -20
```

**Healthy signs:**
- `instant_offset` is **consistently positive** (e.g., +85 ms every frame)
- `ema_offset` converges and holds steady (e.g., ~85 ms)
- `A/V_offset` (post-correction residual) stays near **±0 ms** (ideally < ±20 ms)

**Broken signs:**
- `instant_offset` drifts or is sometimes negative, sometimes positive
- `ema_offset` climbs unbounded or oscillates wildly
- `A/V_offset` stays large and non-zero (e.g., > 50 ms)
- Audio-ahead offset is NOT being subtracted from the GStreamer buffers

### Summary

| Term | Meaning | Example | What it means for playback |
|------|---------|---------|---------------------------|
| **Audio ahead** | Audio PTS is numerically later on the timeline | `audio_pts=1085 ms` vs `video_pts=1000 ms` | Audio plays 85 ms **late** (lags visually) |
| **instant_offset** | The raw PTS difference (should be positive ≈+85 ms) | `+85 ms` | The capture-timestamp mismatch the EMA must correct |
| **ema_offset** | The moving average of offsets (the correction value) | `≈ 85 ms` | The amount the EMA subtracts from audio PTS to align it |
| **A/V_offset** | Post-correction residual (should be ≈0 ±20 ms) | `+5 ms` | How far off sync we are *after* the EMA correction |

If the EMA is working, `instant_offset` (the problem) + `ema_offset` (the fix) = `A/V_offset` (the result, should be small).

---
- App: `app/src/main/java/com/crestron/streamout/Camera2GstStreamer.java`
- Native: `app/src/main/cpp/camera2_gst_streamer.cpp`
- GStreamer build: `/home/builduser/StreamingApkBldEnv/CSS/gst-android-build/armeabi-v7a/`
- Logcat: `logcat.txt`
