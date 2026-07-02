package com.crestron.streamout;

import android.content.Context;
import android.content.pm.PackageManager;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.SessionConfiguration;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTimestamp;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.media.MediaRecorder;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.util.Range;
import android.util.Size;
import android.view.Surface;

import androidx.annotation.NonNull;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Captures video from Camera2 and streams it over RTSP via a GStreamer pipeline.
 *
 * Zero-copy path:
 *   Camera2 DMA -> MediaCodec Surface input (H.264/H.265 HW encoder)
 *       -> JNI -> GStreamer appsrc -> h264parse/h265parse
 *       -> rtph264pay/rtph265pay -> GstRTSPServer
 *
 * Default codec is H.264. Call {@link #setPreferH265(boolean) setPreferH265(true)}
 * before {@link #start} to select H.265 instead.
 *
 * RTSP URL: {@code rtsp://<device-ip>:<port>/camera.sdp}
 */
public class Camera2GstStreamer {

    private static final String TAG = "strmout_Camera2GstStreamer";

    private static final String MIME_H265 = "video/hevc";
    private static final String MIME_H264 = "video/avc";

    // Qualcomm QCS8250 (x80) hardware encoder names, preferred first.
    // OMX.qcom.* is listed before c2.qti.* because on this firmware the C2/CCodec path
    // can trigger C2OMXNode::onDataSpaceChanged returning C2_CORRUPTED on the first
    // camera buffer, which propagates as MediaCodec error 0x80000000.
    private static final String[] HEVC_HW_ENCODERS = {
            "OMX.qcom.video.encoder.hevc",
            "c2.qti.hevc.encoder",
    };
    private static final String[] AVC_HW_ENCODERS = {
            "OMX.qcom.video.encoder.avc",
            "c2.qti.avc.encoder",
    };

    // -------------------------------------------------------------------------
    // Stream mode – controls which capture paths and pipeline branches are active
    // -------------------------------------------------------------------------

    /** Controls which media streams are captured and served over RTSP. */
    public enum StreamMode {
        /** Capture and stream video only (no audio). */
        VIDEO_ONLY,
        /** Capture and stream audio only (no video). */
        AUDIO_ONLY,
        /** Capture and stream both video and audio (default). */
        VIDEO_AND_AUDIO_BOTH
    }

    /**
     * Controls the encoder GOP (Group of Pictures) structure.
     * <ul>
     *   <li>{@link GopMode#ALL_INTRA}  – every frame is an IDR (no P-frames).
     *       Maximum resilience to packet loss; requires ~2× the bitrate for
     *       equivalent quality.</li>
     *   <li>{@link GopMode#SHORT_GOP}  – IDR every {@link #mIFrameInterval} seconds
     *       followed by P-frames.  Better quality at the same bitrate; decoder
     *       corruption window is limited to one GOP on packet loss.
     *       Default: {@link #DEFAULT_I_FRAME_INTERVAL} second(s).</li>
     * </ul>
     */
    public enum GopMode {
        /** Every frame is an IDR – maximum loss resilience, higher bitrate. */
        ALL_INTRA,
        /** Short GOP with configurable IDR interval – better quality at fixed bitrate. */
        SHORT_GOP
    }

    /** Default IDR interval (seconds) used by {@link GopMode#SHORT_GOP}. */
    public static final int DEFAULT_I_FRAME_INTERVAL = 30;

    /**
     * Controls the RTP lower-transport protocol negotiated during RTSP SETUP.
     * Applies to both the video and audio streams in the session.
     * <ul>
     *   <li>{@link TransportMode#TCP} – RTP is interleaved over the RTSP control
     *       TCP connection (RFC 2326 §10.12).  No extra ports are opened; works
     *       through NAT and firewalls.  Default.</li>
     *   <li>{@link TransportMode#UDP} – RTP/RTCP use separate UDP unicast sockets.
     *       Lower per-packet overhead; requires reachable UDP ports between server
     *       and client.</li>
     * </ul>
     * Must be called before {@link #start}.
     */
    public enum TransportMode {
        /** RTP interleaved over the RTSP TCP connection (default). */
        TCP,
        /** RTP over UDP unicast sockets. */
        UDP
    }

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    private final Context        mContext;
    private int                  mPort;
    private int                  mWidth;
    private int                  mHeight;
    private int                  mFrameRate;
    private int                  mBitRate;
    /** Prefer H.265 (true, default) over H.264 (false).  Static so it can be set
     *  externally (e.g. from configure.txt or gRPC) before a new
     *  Camera2GstStreamer instance starts, via {@link #setPreferH265(boolean)}. */
    private static boolean       m_preferH265    = true; // H.265 is the default
    /** Optional upper bound on the encoder bitrate in bps; {@code 0} disables the
     *  cap.  Static so it can be set externally (e.g. from configure.txt or gRPC)
     *  before a new Camera2GstStreamer instance starts, via
     *  {@link #setMaxBitRate(int)}. */
    private static int           m_maxBitRate    = 0; // 0 = no cap
    /** Which media streams to capture/serve.  Static so it can be set externally
     *  (e.g. from configure.txt or gRPC) before a new Camera2GstStreamer instance
     *  starts, via {@link #setStreamMode(int)}. */
    private static StreamMode    mStreamMode     = StreamMode.VIDEO_AND_AUDIO_BOTH;
    private GopMode              mGopMode        = GopMode.SHORT_GOP;
    private int                  mIFrameInterval = DEFAULT_I_FRAME_INTERVAL;
    private TransportMode        mTransportMode  = TransportMode.TCP;
    private String               mMime;                // set by findHardwareEncoder()

    // -------------------------------------------------------------------------
    // Feature flags – all disabled/conservative by default.
    // Set via the public setters before calling start().
    // -------------------------------------------------------------------------
    /** Pin GMainLoop + encoder callback threads to the same high-performance CPU core. */
    private boolean mCpuAffinityEnable    = false;
    /** Set TCP_NODELAY on the RTSP socket to disable Nagle's algorithm. */
    private boolean mTcpNoDelay             = false;
    /** Shrink the kernel send buffer to this many bytes to limit stale-frame queuing.
     *  0 = leave at kernel default. Recommended: 65536 (64 KB ≈ 4 frames at 20 Mbps/30fps). */
    private int     mSoSndbufSize           = 0;
    /** Set TCP_NOTSENT_LOWAT to cap unsent-data backlog and apply back-pressure.
     *  0 = disabled. Recommended: 16384 (16 KB). Requires Android 7+ (Linux 4.4+). */
    private int     mTcpNotsentLowat        = 0;
    /** Mark packets with DSCP EF (0xb8) for priority queuing on managed networks. */
    private boolean mIpTosDscpEf            = false;
    /** Include h264parse/h265parse in the GStreamer pipeline.
     *  false (default) = no parser, lowest latency.
     *  true            = parser included (adds ~1 frame latency, more robust caps negotiation). */
    private boolean mH26xParseEnable      = false;
    /** Set rtpbin buffer-mode=none to disable sender-side jitter buffering.
     *  true (default) = buffer-mode=none applied (lowest latency).
     *  false          = buffer-mode left at GStreamer default (slave). */
    private boolean mRtpbinBufferModeNone = true;
    /** Fire periodic 30-second element-property dump to logcat.
     *  Default: disabled (false) to avoid logcat noise in production. */
    private boolean mPeriodicDumpEnable   = false;
    /** Number of NAL slices per encoded frame (Qualcomm vendor extension).
     *  0 (default) = disabled; typical value = 4.
     *  Splitting frames into slices lets the decoder start reconstruction
     *  before the full frame arrives, reducing end-to-end latency. */
    private int     mEncSliceCount        = 0;

    public void setCpuAffinityEnable(boolean enable)    { mCpuAffinityEnable    = enable; }
    public void setTcpNoDelay(boolean enable)           { mTcpNoDelay           = enable; }
    public void setSoSndbufSize(int bytes)              { mSoSndbufSize         = bytes; }
    public void setTcpNotsentLowat(int bytes)           { mTcpNotsentLowat      = bytes; }
    public void setIpTosDscpEf(boolean enable)          { mIpTosDscpEf          = enable; }
    public void setH26xParseEnable(boolean enable)      { mH26xParseEnable      = enable; }
    public void setRtpbinBufferModeNone(boolean enable) { mRtpbinBufferModeNone = enable; }
    public void setPeriodicDumpEnable(boolean enable)   { mPeriodicDumpEnable   = enable; }
    public void setEncSliceCount(int count)              { mEncSliceCount        = Math.max(0, count); }

    /** Custom GStreamer pipeline string.  When non-empty, it overrides the
     *  auto-generated pipeline in {@link #initGstPipeline()}.  Set via
     *  {@link #setCustomPipeline(String)}; empty string (default) = auto-generate. */
    private static String m_customePipeline = "";

    /**
     * Override the auto-generated GStreamer pipeline with a custom launch string.
     * When set to a non-empty value, {@link #initGstPipeline()} uses it verbatim
     * instead of building one from the stream mode / codec settings.
     * Pass an empty string to revert to auto-generation.
     */
    public static void setCustomPipeline(String pipeline) {
        m_customePipeline = (pipeline != null) ? pipeline : "";

        Log.i(TAG, "setCustomPipeline: " + m_customePipeline);
    }

    /** Advertise RTCP feedback (a=rtcp-fb nack/pli/fir) in the RTSP SDP and
     *  restrict the media factory to the AVPF profile (single m=video).  When
     *  false the server advertises plain RTP/AVP only.  Static so it can be set
     *  externally (e.g. from configure.txt or gRPC) before a new
     *  Camera2GstStreamer instance starts; applied to native in
     *  {@link #initGstPipeline()} before pipeline init.  Default: true (AVPF). */
    private static boolean m_rtcpFeedback = true;

    /**
     * Enable/disable RTCP feedback advertisement in the RTSP SDP.
     * Takes effect on the next {@code start()} (the RTSP factory reads the flag
     * at pipeline-init time), not on a stream that is already running.
     */
    public static void setRtcpFeedback(boolean enable) {
        m_rtcpFeedback = enable;
        Log.i(TAG, "setRtcpFeedback: " + m_rtcpFeedback);
    }

    private MediaCodec           mEncoder;
    private Surface              mEncoderSurface;

    private CameraManager        mCameraManager;
    private CameraDevice         mCameraDevice;
    private CameraCaptureSession mCaptureSession;
    private HandlerThread        mCameraThread;
    private Handler              mCameraHandler;

    private String               mCameraId;
    private final AtomicBoolean  mRunning             = new AtomicBoolean(false);
    private volatile boolean     mGstPipelineStarted  = false;

    private static final int     MAX_ENCODER_RESTARTS = 3;
    private final AtomicInteger  mRestartCount        = new AtomicInteger(0);

    // All-intra safety net: request REQUEST_SYNC_FRAME every N frames.
    // In all-intra mode every frame IS an IDR so this counter never reaches
    // the threshold in normal operation.  It acts as a safety net in case the
    // encoder firmware ignores KEY_I_FRAME_INTERVAL=0 and slips into IBP mode.
    private static final int     FORCED_IDR_INTERVAL_FRAMES = 150;  // ~5 s at 30fps fallback
    private final AtomicInteger  mFramesSinceIDR      = new AtomicInteger(0);

    // Cached CSD (VPS/SPS/PPS for H.265; SPS/PPS for H.264).
    // Re-injected into the pipeline when a new RTSP client connects so the
    // parser has parameter sets before the first IDR arrives.
    private volatile ByteBuffer  mCachedCsd           = null;

    // Set to true when a new RTSP client connects (nativeConsumeKeyframeRequest
    // returns true).  While true, P/B frames are withheld from the pipeline;
    // only the requested IDR (and its preceding CSD re-injection) is forwarded.
    // This prevents h264parse/h265parse from logging "broken/invalid nal" for
    // every P-frame that arrives before the encoder delivers the next IDR.
    private volatile boolean     mWaitingForIDR       = false;

    // -------------------------------------------------------------------------
    // Audio capture state
    // -------------------------------------------------------------------------

    /** Audio capture sample rate in Hz.  Default 48000 (Opus is the default
     *  audio codec and operates at 48 kHz internally); settable at runtime to
     *  44100 (or any AAC-supported rate) via {@link #setAudioSampleRate(int)}
     *  before a new instance starts (e.g. from the native cam2_streamer_debug
     *  SET_AUDIO_SAMPLE_RATE command).  Independent of the channel count. */
    private static int          AUDIO_SAMPLE_RATE      = 48000;
    /** Audio channel count.  Default 1 (mono); settable at runtime to 2 (stereo)
     *  via {@link #setAudioChannelCount(int)} before a new instance starts (e.g.
     *  from the native cam2_streamer_debug SET_AUDIO_CHANNELS command).
     *  Independent of the sample rate. */
    private static int          AUDIO_CHANNEL_COUNT    = 1;       // mono (default)
    private static final int    AUDIO_PCM_FRAME_BYTES  = 2;       // 16-bit PCM
    private static final int    AUDIO_AAC_BITRATE      = 64_000;  // 64 kbps AAC-LC
    private static final String MIME_AAC               = "audio/mp4a-latm";

    // Qualcomm QCS8250 (x80) hardware AAC encoder names, preferred first.
    private static final String[] AAC_HW_ENCODERS = {
            "OMX.qcom.audio.encoder.aac",
            "c2.qti.aac.encoder",
    };

    // -------------------------------------------------------------------------
    // Opus audio path (alternative to AAC)
    // -------------------------------------------------------------------------
    /** When {@code true} the audio branch streams Opus instead of AAC: raw PCM
     *  is pushed straight to GStreamer ({@code appsrc → audioconvert → opusenc →
     *  rtpopuspay}) instead of being AAC-encoded in MediaCodec.  This skips the
     *  MediaCodec software AAC encoder entirely (lower latency).  Default
     *  {@code true} (Opus is the default audio codec).  Toggle via
     *  {@link #setUseOpus(boolean)} (native {@code SET_USE_OPUS}) before a new
     *  stream starts.  Enabling Opus forces {@link #AUDIO_SAMPLE_RATE} to 48000
     *  (Opus operates internally at 48 kHz). */
    private static boolean      m_useOpus              = true; // Opus is the default
    /** Opus encoder frame size in ms.  This is ALSO the PCM read granularity, so
     *  opusenc always receives an integer number of frames.  10 ms = lowest
     *  practical latency; 20 ms = slightly better quality/efficiency.  Valid
     *  opusenc frame sizes: 2.5 / 5 / 10 / 20 / 40 / 60. */
    private static final int    OPUS_FRAME_MS          = 10;
    private static final int    OPUS_BITRATE           = 64_000;  // 64 kbps (matches AAC)
    private static final int    OPUS_RTP_PAYLOAD_TYPE  = 97;      // same RTP pt slot as AAC
    /** Master on/off for the Java-side "Opus push latency" diagnostic print in
     *  {@link #runOpusPushLoop()}.  Default {@code false} (no logging).  Toggle
     *  via {@link #setAudioLatencyDebug(boolean)} (native
     *  {@code SET_AUDIO_LATENCY_DEBUG}).  LOGGING ONLY — audio capture/push keep
     *  running; this just gates the periodic {@code Log.i}. */
    private static volatile boolean m_audioLatencyDebug = false;

    private AudioRecord      mAudioRecord;
    private MediaCodec       mAudioEncoder;
    private Thread           mAudioThread;
    private volatile boolean mAudioRunning = false;
/*******Measure the aac encoder pipeline latency************************************************************************/
    // [AAC-LATENCY] Debug instrumentation for measuring AAC encoder pipeline
    // latency.  Maps each input PCM chunk's PTS (us, CLOCK_BOOTTIME) to the
    // wall-clock instant (us, CLOCK_BOOTTIME) it was handed to queueInputBuffer().
    // At dequeueOutputBuffer() we floor-match the output frame's PTS to find the
    // input chunk it came from and subtract.  Stays tiny (encoder depth ~1-2
    // frames); size-capped below.  Audio thread only, so no synchronization.
    //// private final java.util.TreeMap<Long,Long> mAacInWallUs = new java.util.TreeMap<>();
    //// private long mAacLatCount = 0;
/*******Measure the encoder pipeline latency************************************************************************/
    // -------------------------------------------------------------------------
    // JNI - implemented in camera2_gst_streamer.cpp
    // -------------------------------------------------------------------------

    private static native boolean nativeGstPipelineInit(String pipeline, int port, int transport);
    private static native void    nativePushEncodedBuffer(
            ByteBuffer buf, int offset, int size,
            long ptsUs, boolean isKeyFrame, boolean isCsd);
        private static native void    nativePushAudioBuffer(
            ByteBuffer buf, int offset, int size, long ptsUs);
    private static native void    nativeGstPipelineDestroy();
    private static native boolean nativeConsumeKeyframeRequest();
    private static native void    nativeSetCpuAffinityEnable(boolean enable);
    private static native void    nativeSetTcpNoDelay(boolean enable);
    private static native void    nativeSetSoSndbufSize(int bytes);
    private static native void    nativeSetTcpNotsentLowat(int bytes);
    private static native void    nativeSetIpTosDscpEf(boolean enable);
    private static native void    nativeSetH26xParseEnable(boolean enable);
    private static native void    nativeSetRtpbinBufferModeNone(boolean enable);
    private static native void    nativeSetPeriodicDumpEnable(boolean enable);
    private static native void    nativeSetRtcpFeedback(boolean enable);

    // -------------------------------------------------------------------------
    // Camera capability descriptor
    // -------------------------------------------------------------------------

    /** Immutable snapshot of a single camera's identity and best video capability. */
    public static final class CameraInfo {
        /** Camera2 ID string (e.g. "0", "1"). */
        public final String         id;
        /** LENS_FACING_FRONT / LENS_FACING_BACK / LENS_FACING_EXTERNAL, or -1 if unknown. */
        public final int            facing;
        /** Human-readable facing label. */
        public final String         facingLabel;
        /** All output sizes supported for SurfaceTexture / MediaCodec input, largest first. */
        public final List<Size>     supportedSizes;
        /** Largest supported size (highest pixel count). */
        public final Size           bestSize;
        /** Maximum stable AE frame-rate range available. */
        public final Range<Integer> bestFpsRange;
        /** Physical sensor pixel array size (informational). */
        public final Size           sensorArraySize;

        CameraInfo(String id, int facing, List<Size> sizes,
                   Range<Integer> fpsRange, Size sensorArraySize) {
            this.id              = id;
            this.facing          = facing;
            this.facingLabel     = facingLabel(facing);
            this.supportedSizes  = Collections.unmodifiableList(sizes);
            this.bestSize        = sizes.isEmpty() ? new Size(1280, 720) : sizes.get(0);
            this.bestFpsRange    = fpsRange;
            this.sensorArraySize = sensorArraySize;
        }

        static String facingLabel(int f) {
            switch (f) {
                case CameraCharacteristics.LENS_FACING_FRONT:    return "FRONT";
                case CameraCharacteristics.LENS_FACING_BACK:     return "BACK";
                case CameraCharacteristics.LENS_FACING_EXTERNAL: return "EXTERNAL";
                default: return "UNKNOWN(" + f + ")";
            }
        }

        @Override
        public String toString() {
            return "Camera{id=" + id
                    + " " + facingLabel
                    + " best=" + bestSize.getWidth() + "x" + bestSize.getHeight()
                    + " fps=" + bestFpsRange
                    + " sizes=" + supportedSizes.size() + "}";
        }
    }

    // -------------------------------------------------------------------------
    // Static camera availability tracking
    // -------------------------------------------------------------------------

    /** Set of camera IDs currently reported as available by the system. */
    private static final Set<String> sAvailableCameras =
            ConcurrentHashMap.newKeySet();

    /** True once {@link #registerCameraAvailability} has been called. */
    private static volatile boolean sAvailabilityRegistered = false;

    private static CameraManager.AvailabilityCallback sAvailabilityCallback;

    /** When true, camera availability callbacks automatically start/stop streaming. */
    private static volatile boolean sAutoStartStop = false;

    /** Weak reference to the service so callbacks can call startStreaming/stopStreaming. */
    private static volatile StreamOutSvcCtrl sServiceRef = null;

    /**
     * Register a {@link CameraManager.AvailabilityCallback} to track which
     * cameras are available in real time.  Call once from
     * {@code Service.onCreate()} or {@code Service.onStartCommand()} – before
     * any {@link Camera2GstStreamer} instance is created.
     *
     * <p>Thread-safe; subsequent calls are no-ops.
     */
    public static void registerCameraAvailability(@NonNull Context context) {
        if (sAvailabilityRegistered) {
            Log.d(TAG, "registerCameraAvailability: already registered");
            return;
        }
        if (context instanceof StreamOutSvcCtrl) {
            sServiceRef = (StreamOutSvcCtrl) context;
        }
        CameraManager mgr =
                (CameraManager) context.getSystemService(Context.CAMERA_SERVICE);

        // Seed the set with currently known cameras.
        try {
            String[] ids = mgr.getCameraIdList();
            for (String id : ids) {
                sAvailableCameras.add(id);
            }
            Log.i(TAG, "registerCameraAvailability: seeded "
                    + ids.length + " camera(s): " + Arrays.toString(ids));
        } catch (CameraAccessException e) {
            Log.e(TAG, "registerCameraAvailability: getCameraIdList failed: "
                    + e.getMessage(), e);
        }

        sAvailabilityCallback = new CameraManager.AvailabilityCallback() {
            @Override
            public void onCameraAvailable(@NonNull String cameraId) {
                sAvailableCameras.add(cameraId);
                Log.i(TAG, "onCameraAvailable: id=" + cameraId
                        + " available=" + sAvailableCameras);
                StreamOutSvcCtrl svc = sServiceRef;
                if (svc != null && sAutoStartStop) {
                    svc.startStreaming(0);
                }
            }

            @Override
            public void onCameraUnavailable(@NonNull String cameraId) {
                // Check if camera is still physically present in the system.
                // If yes → opened by an app (us or another) → ignore.
                // If no  → physically disconnected (USB-C unplug) → stop streaming.
                boolean stillPresent = false;
                try {
                    for (String id : mgr.getCameraIdList()) {
                        if (id.equals(cameraId)) { stillPresent = true; break; }
                    }
                } catch (CameraAccessException e) {
                    Log.e(TAG, "onCameraUnavailable: getCameraIdList failed: "
                            + e.getMessage());
                }
                if (stillPresent) {
                    Log.d(TAG, "onCameraUnavailable: id=" + cameraId
                            + " still in camera list – opened by an app, ignoring");
                    return;
                }
                // Camera physically removed.
                sAvailableCameras.remove(cameraId);
                Log.w(TAG, "onCameraUnavailable: id=" + cameraId
                        + " physically disconnected – available=" + sAvailableCameras);
                StreamOutSvcCtrl svc = sServiceRef;
                if (svc != null && sAutoStartStop) {
                    svc.stopStreaming(0);
                }
            }
        };

        mgr.registerAvailabilityCallback(sAvailabilityCallback, null);
        sAvailabilityRegistered = true;
        Log.i(TAG, "registerCameraAvailability: callback registered");
    }

    /**
     * Unregister the availability callback.  Call from
     * {@code Service.onDestroy()} if desired; safe to omit for a persistent
     * foreground service.
     */
    public static void unregisterCameraAvailability(@NonNull Context context) {
        if (!sAvailabilityRegistered || sAvailabilityCallback == null) return;
        CameraManager mgr =
                (CameraManager) context.getSystemService(Context.CAMERA_SERVICE);
        mgr.unregisterAvailabilityCallback(sAvailabilityCallback);
        sAvailabilityRegistered = false;
        Log.i(TAG, "unregisterCameraAvailability: callback unregistered");
    }

    /**
     * Returns {@code true} if the given camera ID is currently reported as
     * available by the system.  If {@link #registerCameraAvailability} has not
     * been called yet, falls back to a one-shot
     * {@link CameraManager#getCameraIdList()} check.
     */
    public static boolean isCameraAvailable(@NonNull Context context, @NonNull String cameraId) {
        if (sAvailabilityRegistered) {
            return sAvailableCameras.contains(cameraId);
        }
        // Fallback: callback not registered, do a synchronous check.
        CameraManager mgr =
                (CameraManager) context.getSystemService(Context.CAMERA_SERVICE);
        try {
            for (String id : mgr.getCameraIdList()) {
                if (id.equals(cameraId)) return true;
            }
        } catch (CameraAccessException e) {
            Log.e(TAG, "isCameraAvailable: " + e.getMessage(), e);
        }
        return false;
    }

    /**
     * Returns the set of camera IDs currently known to be available.
     * Empty if {@link #registerCameraAvailability} was never called and no
     * cameras are found.
     */
    public static Set<String> getAvailableCameraIds() {
        return Collections.unmodifiableSet(sAvailableCameras);
    }

    // -------------------------------------------------------------------------
    // Constructor
    // -------------------------------------------------------------------------

    public Camera2GstStreamer(@NonNull Context context) {
        mContext = context;
    }

    // -------------------------------------------------------------------------
    // Codec selection
    // -------------------------------------------------------------------------

    /**
     * Select H.265 (true, default) or H.264 (false) encoding.
     * Must be called before {@link #start}.  Static so it can be set externally
     * (e.g. from configure.txt or gRPC) before a new instance starts.
     */
    public static void setPreferH265(boolean prefer) {
        m_preferH265 = prefer;
        Log.i(TAG, "setPreferH265: " + m_preferH265);
    }

    /**
     * Set an upper bound (in bps) on the encoder bitrate; pass {@code 0} to disable
     * the cap.  Applied after the requested/auto bitrate is resolved.
     * Must be called before {@link #start}.  Static so it can be set externally
     * (e.g. from configure.txt or gRPC) before a new instance starts.
     */
    public static void setMaxBitRate(int bps) {
        m_maxBitRate = (bps > 0) ? bps : 0;
        Log.i(TAG, "setMaxBitRate: " + m_maxBitRate);
    }

    /**
     * Select the GOP mode and (for {@link GopMode#SHORT_GOP}) the IDR interval.
     * <ul>
     *   <li>{@link GopMode#ALL_INTRA} – pass any value for {@code iFrameIntervalSec};
     *       it is ignored.</li>
     *   <li>{@link GopMode#SHORT_GOP} – {@code iFrameIntervalSec} sets
     *       {@link android.media.MediaFormat#KEY_I_FRAME_INTERVAL}.
     *       Use {@link #DEFAULT_I_FRAME_INTERVAL} (1 s) for the recommended default.</li>
     * </ul>
     * Must be called before {@link #start}.
     */
    public void setGopMode(GopMode mode, int iFrameIntervalSec) {
        mGopMode        = mode;
        mIFrameInterval = (mode == GopMode.ALL_INTRA) ? 0
                        : Math.max(1, iFrameIntervalSec);
    }

    /**
     * Select the stream mode.
     * <ul>
     *   <li>{@link StreamMode#VIDEO_ONLY}           – camera video encoder only, no microphone</li>
     *   <li>{@link StreamMode#AUDIO_ONLY}           – microphone audio encoder only, no camera</li>
     *   <li>{@link StreamMode#VIDEO_AND_AUDIO_BOTH} – both video and audio (default)</li>
     * </ul>
     * Must be called before {@link #start}.  Static so it can be set externally
     * (e.g. from configure.txt or gRPC) before a new instance starts.
     */
    public static void setStreamMode(StreamMode mode) {
        mStreamMode = mode;
        Log.i(TAG, "setStreamMode: " + mStreamMode);
    }

    /**
     * Select the stream mode by ordinal, for external callers (configure.txt / gRPC /
     * native debug) that pass a plain integer:
     * {@code 0 = VIDEO_ONLY, 1 = AUDIO_ONLY, 2 = VIDEO_AND_AUDIO_BOTH}.
     * Out-of-range values are ignored.  Must be called before {@link #start}.
     */
    public static void setStreamMode(int mode) {
        StreamMode[] values = StreamMode.values();
        if (mode < 0 || mode >= values.length) {
            Log.w(TAG, "setStreamMode: invalid ordinal " + mode + " (ignored)");
            return;
        }
        setStreamMode(values[mode]);
    }

    /**
     * Set the audio capture sample rate in Hz.  Independent of the channel count,
     * so any rate/channel combination can be tested (e.g. 48000 mono).  Accepts
     * AAC-LC supported rates (8000…96000); other values are ignored.  Must be
     * called before {@link #start}.  Static so it can be set externally (e.g.
     * from the native cam2_streamer_debug SET_AUDIO_SAMPLE_RATE command) before a
     * new instance starts.
     */
    public static void setAudioSampleRate(int rate) {
        if (aacFreqIndex(rate) < 0) {
            Log.w(TAG, "setAudioSampleRate: unsupported rate " + rate + " (ignored)");
            return;
        }
        AUDIO_SAMPLE_RATE = rate;
        Log.i(TAG, "setAudioSampleRate: " + AUDIO_SAMPLE_RATE);
    }

    /**
     * Set the audio channel count: {@code 1} = mono, {@code 2} = stereo.
     * Independent of the sample rate.  Other values are ignored.  Must be called
     * before {@link #start}.  Static so it can be set externally (e.g. from the
     * native cam2_streamer_debug SET_AUDIO_CHANNELS command) before a new
     * instance starts.
     */
    public static void setAudioChannelCount(int channels) {
        if (channels != 1 && channels != 2) {
            Log.w(TAG, "setAudioChannelCount: invalid channels " + channels
                    + " (use 1=mono or 2=stereo; ignored)");
            return;
        }
        AUDIO_CHANNEL_COUNT = channels;
        Log.i(TAG, "setAudioChannelCount: " + AUDIO_CHANNEL_COUNT);
    }

    /**
     * Enable or disable the Opus audio path.  When enabled, audio is sent as
     * Opus (raw PCM → GStreamer {@code opusenc → rtpopuspay}) instead of AAC,
     * bypassing the MediaCodec software AAC encoder.  Enabling Opus forces the
     * capture sample rate to 48000 Hz (Opus operates at 48 kHz internally).
     * Must be called before {@link #start}.  Static so it can be set externally
     * (e.g. from the native cam2_streamer_debug {@code SET_USE_OPUS} command)
     * before a new instance starts.
     */
    public static void setUseOpus(boolean enable) {
        m_useOpus = enable;
        if (enable && AUDIO_SAMPLE_RATE != 48000) {
            AUDIO_SAMPLE_RATE = 48000;
            Log.i(TAG, "setUseOpus: forcing AUDIO_SAMPLE_RATE=48000 for Opus");
        }
        Log.i(TAG, "setUseOpus: " + m_useOpus + " (rate=" + AUDIO_SAMPLE_RATE + ")");
    }

    /**
     * Enable or disable the audio-latency diagnostic logging (the periodic
     * "Opus push latency" print emitted by {@link #runOpusPushLoop()}).  This is
     * the Java half of the {@code SET_AUDIO_LATENCY_DEBUG} debug command; the
     * native half ({@code Cam2Streamer_SetAudioLatencyDebug}) gates the "Opus
     * encode latency" probe and "[A/V sync]" prints.  Logging only — audio
     * capture, push, and the EMA PTS correction are unaffected.  Can be toggled
     * at any time (takes effect on the next 100-frame log boundary).
     */
    public static void setAudioLatencyDebug(boolean enable) {
        m_audioLatencyDebug = enable;
        Log.i(TAG, "setAudioLatencyDebug: " + m_audioLatencyDebug);
    }

    /**
     * Returns a human-readable, multi-line summary of every externally-settable
     * static configuration value (the ones the native cam2_streamer_debug SET_*
     * commands change).  This is the <em>next-start intent</em> — the config that
     * will be applied the next time {@link #start} runs — not the live GStreamer
     * element state (use the {@code PRINT_ELEMENT_PROPERTY} debug command for the
     * running pipeline).  Called from native via the {@code DUMP_ALL_CONFIG}
     * debug command.
     */
    public static String getConfigSummary() {
        int freqIdx = aacFreqIndex(AUDIO_SAMPLE_RATE);
        String aacCodecData = (freqIdx >= 0)
                ? String.format("%04x",
                        (2 << 11) | (freqIdx << 7) | (AUDIO_CHANNEL_COUNT << 3))
                : "n/a";
        return "Camera2GstStreamer config (next-start intent):"
             + "\n  preferH265      = " + m_preferH265
             + "\n  maxBitRate      = " + m_maxBitRate
                 + (m_maxBitRate == 0 ? " (no cap)" : " bps")
             + "\n  streamMode      = " + mStreamMode
             + "\n  audioCodec      = " + (m_useOpus ? "Opus" : "AAC")
             + "\n  audioSampleRate = " + AUDIO_SAMPLE_RATE + " Hz"
             + "\n  audioChannels   = " + AUDIO_CHANNEL_COUNT
                 + " (" + (AUDIO_CHANNEL_COUNT == 2 ? "stereo" : "mono") + ")"
             + "\n  aacBitRate      = " + AUDIO_AAC_BITRATE + " bps"
             + "\n  aacCodecData    = 0x" + aacCodecData
             + "\n  rtcpFeedback    = " + m_rtcpFeedback
             + "\n  customPipeline  = "
                 + (m_customePipeline.isEmpty() ? "(none)" : m_customePipeline);
    }

    /**
     * Select the RTP lower-transport protocol for the RTSP session.
     * Applies to both video and audio streams.
     * <p>
     * {@link TransportMode#TCP} (default) interleaves RTP inside the RTSP
     * control connection; {@link TransportMode#UDP} uses separate UDP sockets.
     * </p>
     * Must be called before {@link #start}.
     */
    public void setTransportMode(TransportMode mode) {
        mTransportMode = mode;
    }

    // -------------------------------------------------------------------------
    // Camera enumeration
    // -------------------------------------------------------------------------

    /**
     * Queries Camera2 for all cameras and their capabilities.
     *
     * @return List of {@link CameraInfo} sorted by descending best-size pixel count.
     */
    public static List<CameraInfo> listCameras(@NonNull Context context) {
        CameraManager mgr =
                (CameraManager) context.getSystemService(Context.CAMERA_SERVICE);
        List<CameraInfo> result = new ArrayList<>();

        String[] ids;
        try {
            ids = mgr.getCameraIdList();
        } catch (CameraAccessException e) {
            Log.e(TAG, "listCameras: " + e.getMessage(), e);
            return result;
        }

        Log.i(TAG, "listCameras: found " + ids.length + " camera(s)");

        for (String id : ids) {
            try {
                CameraCharacteristics ch = mgr.getCameraCharacteristics(id);

                Integer facing  = ch.get(CameraCharacteristics.LENS_FACING);
                int facingVal   = facing != null ? facing : -1;
                Size sensorSize = ch.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE);

                // READ-ONLY diagnostic: which clock does the camera HAL stamp buffers with?
                //   TIMESTAMP_SOURCE_UNKNOWN  (0) -> CLOCK_MONOTONIC  (== System.nanoTime())
                //   TIMESTAMP_SOURCE_REALTIME (1) -> CLOCK_BOOTTIME   (== elapsedRealtimeNanos())
                // This decides whether the audio PTS (line ~1688) should use
                // System.nanoTime() (to match MONOTONIC) or stay on
                // elapsedRealtimeNanos() (to match BOOTTIME).  Pure logging; no behaviour change.
                Integer tsSrc = ch.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE);
                Log.i(TAG, "  id=" + id + " SENSOR_INFO_TIMESTAMP_SOURCE=" + tsSrc
                        + (tsSrc != null
                            && tsSrc == CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME
                                ? " (REALTIME / BOOTTIME / elapsedRealtimeNanos)"
                                : " (UNKNOWN / MONOTONIC / System.nanoTime)"));

                StreamConfigurationMap map =
                        ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);

                List<Size> sizes = new ArrayList<>();
                if (map != null) {
                    android.util.Size[] raw =
                            map.getOutputSizes(android.graphics.SurfaceTexture.class);
                    if (raw != null) {
                        for (android.util.Size s : raw)
                            sizes.add(new Size(s.getWidth(), s.getHeight()));
                    }
                    android.util.Size[] codecSizes = map.getOutputSizes(MediaCodec.class);
                    if (codecSizes != null) {
                        for (android.util.Size s : codecSizes) {
                            Size c = new Size(s.getWidth(), s.getHeight());
                            if (!sizes.contains(c)) sizes.add(c);
                        }
                    }
                }
                Collections.sort(sizes,
                        (a, b) -> (b.getWidth() * b.getHeight()) - (a.getWidth() * a.getHeight()));

                Range<Integer>[] fpsRanges =
                        ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
                Range<Integer> bestFps = pickBestFpsRange(fpsRanges);

                Log.i(TAG, "  id=" + id
                        + " " + CameraInfo.facingLabel(facingVal)
                        + " best=" + (sizes.isEmpty() ? "n/a"
                                : sizes.get(0).getWidth() + "x" + sizes.get(0).getHeight())
                        + " fps=" + Arrays.toString(fpsRanges)
                        + " totalSizes=" + sizes.size());

                result.add(new CameraInfo(id, facingVal, sizes, bestFps, sensorSize));

            } catch (CameraAccessException e) {
                Log.e(TAG, "listCameras: id=" + id + " failed: " + e.getMessage(), e);
            }
        }
        return result;
    }

    private static Range<Integer> pickBestFpsRange(Range<Integer>[] ranges) {
        if (ranges == null || ranges.length == 0) return new Range<>(30, 30);
        Range<Integer> best = ranges[0];
        for (Range<Integer> r : ranges) {
            if (r.getUpper() > best.getUpper()) best = r;
            else if (r.getUpper().equals(best.getUpper())
                    && r.getLower() > best.getLower()) best = r;
        }
        return best;
    }

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------

    /**
     * Convenience entry-point: enumerates cameras, picks back-facing (fallback to first),
     * uses best resolution at up to 30 fps, starts on port 8555.
     * Bitrate is auto-calculated via {@link #recommendedBitRate} based on resolution,
     * frame rate, and codec (H.264 or H.265).
     *
     * RTSP URL: {@code rtsp://<device-ip>:8555/camera.sdp}
     */
    public void startBestCamera() {
        List<CameraInfo> cameras = listCameras(mContext);
        if (cameras.isEmpty()) {
            Log.e(TAG, "startBestCamera: no cameras found");
            return;
        }

        CameraInfo chosen = cameras.get(0);
        for (CameraInfo c : cameras) {
            if (c.facing == CameraCharacteristics.LENS_FACING_BACK) { chosen = c; break; }
        }

        int frameRate = Math.min(chosen.bestFpsRange.getUpper(), 30);
        Log.i(TAG, "startBestCamera: " + chosen
                + " @" + frameRate + "fps codec=" + (m_preferH265 ? "H.265" : "H.264"));
        start(chosen.id, "0.0.0.0", 8555,
                chosen.bestSize.getWidth(), chosen.bestSize.getHeight(),
                frameRate, 0 /* auto-calculate bitrate */);
    }

    /**
     * Graceful stop. Equivalent to {@link #stop()}; provided for symmetry
     * with {@link #startBestCamera()}.
     */
    public void stopCamera() {
        stop();
    }

    /**
     * Open the camera, configure the encoder, start the GStreamer RTSP server pipeline.
     *
     * @param cameraId  Camera2 camera ID string (e.g. "0").
     * @param listenIp  IP to bind the RTSP server on (use "0.0.0.0" for all interfaces).
     * @param port      RTSP server port.
     * @param width     Encode width in pixels.
     * @param height    Encode height in pixels.
     * @param frameRate Target frame rate (fps).
     * @param bitRate   Target bit rate in bps (e.g. 8_000_000 for 8 Mbps), or {@code 0}
     *                  to auto-calculate based on resolution, frame rate, and codec.
     */
    public synchronized void start(String cameraId,
                                   String listenIp, int port,
                                   int width, int height,
                                   int frameRate, int bitRate) {
        if (mRunning.get()) {
            Log.w(TAG, "start() already running - ignoring");
            return;
        }

        // Pre-flight: check camera availability using the registered callback state.
        if (mStreamMode != StreamMode.AUDIO_ONLY) {
            if (!isCameraAvailable(mContext, cameraId)) {
                Log.e(TAG, "start: cameraId=" + cameraId + " is NOT available"
                        + " (available=" + sAvailableCameras + ")");
                return;
            }
            Log.i(TAG, "start: cameraId=" + cameraId + " confirmed available");
        }

        mCameraId  = cameraId;
        mPort      = port;
        mWidth     = width;
        mHeight    = height;
        mFrameRate = frameRate;
        mBitRate   = bitRate;
        mRestartCount.set(0);

        Log.i(TAG, "start: cameraId=" + cameraId
                + " " + width + "x" + height + "@" + frameRate + "fps"
                + " mode=" + mStreamMode
                + (mStreamMode != StreamMode.AUDIO_ONLY
                        ? " codec=" + (m_preferH265 ? "H.265" : "H.264")
                            + " " + (bitRate / 1_000_000) + "Mbps"
                        : "")
                + " rtsp://" + listenIp + ":" + port + "/camera.sdp");

        try {
            if (mStreamMode != StreamMode.AUDIO_ONLY) {
                initEncoder();
                initCamera(cameraId);
            }
            initGstPipeline();
            if (mStreamMode != StreamMode.VIDEO_ONLY) {
                startAudioCapture();
            }
            mRunning.set(true);
            Log.i(TAG, "start: ready"
                    + (mStreamMode != StreamMode.AUDIO_ONLY ? " codec=" + mMime : "")
                    + " mode=" + mStreamMode
                    + " rtsp://" + listenIp + ":" + port + "/camera.sdp");
        } catch (Exception e) {
            Log.e(TAG, "start failed: " + e.getMessage(), e);
            stop();
        }
    }

    /**
     * Tear down camera, encoder, and GStreamer pipeline. Safe to call from any
     * thread; idempotent.
     */
    public synchronized void stop() {
        mRunning.set(false);
        if (mEncoder == null && mCameraDevice == null && !mGstPipelineStarted) {
            Log.i(TAG, "stop: nothing to release");
            return;
        }
        Log.i(TAG, "stop: releasing resources");
        stopAudioCapture();
        releaseCamera();
        releaseEncoder();
        if (mGstPipelineStarted) {
            nativeGstPipelineDestroy();
            mGstPipelineStarted = false;
        }
        Log.i(TAG, "stop: done");
    }

    // =========================================================================
    // Encoder restart
    // =========================================================================

    /**
     * Re-creates only the MediaCodec encoder and reconnects the camera session.
     * The GStreamer RTSP pipeline is left running so connected clients are not dropped.
     * Called on a dedicated thread when a non-recoverable codec error occurs.
     */
    private synchronized void restartEncoder() {
        Log.i(TAG, "restartEncoder: attempt "
                + mRestartCount.get() + "/" + MAX_ENCODER_RESTARTS);

        if (mCaptureSession != null) {
            try { mCaptureSession.stopRepeating(); } catch (Exception ignored) {}
            mCaptureSession.close();
            mCaptureSession = null;
        }
        releaseEncoder();

        try {
            initEncoder();
            createCaptureSession();
        } catch (Exception ex) {
            Log.e(TAG, "restartEncoder failed: " + ex.getMessage(), ex);
            new Thread(this::stop, "cam2gst-error-stop").start();
            return;
        }
        mRunning.set(true);
        Log.i(TAG, "restartEncoder: done");
    }

    // =========================================================================
    // Step 1 - MediaCodec (hardware H.264/H.265 encoder, Surface input mode)
    // =========================================================================

    private void initEncoder() throws Exception {
        String encoderName = findHardwareEncoder();
        if (encoderName == null) {
            throw new RuntimeException("No hardware H.264/H.265 encoder available");
        }

        // Probe whether the chosen encoder supports Constant Quality (CQ) mode.
        // CQ ignores the bitrate ceiling and instead targets a fixed perceptual
        // quality level (KEY_QUALITY 0–100; 80 = excellent).  This gives the best
        // possible visual fidelity because the encoder allocates exactly as many
        // bits as each frame needs.  Qualcomm OMX/C2 encoders on some firmware
        // versions do not advertise CQ support; in that case fall back to VBR.
        boolean cqSupported = true;  // optimistically assume CQ is supported until we find out otherwise
        int effectiveBitRate = 0;
        Range<Double> achievableFpsRange = null;  // populated by capability query below
        for (MediaCodecInfo info :
                new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (info.isEncoder() && info.getName().equalsIgnoreCase(encoderName)) {
                MediaCodecInfo.CodecCapabilities caps =
                        info.getCapabilitiesForType(mMime);
                MediaCodecInfo.EncoderCapabilities encCaps = caps.getEncoderCapabilities();
                MediaCodecInfo.VideoCapabilities vidCaps   = caps.getVideoCapabilities();

                // --- Encoder capabilities ---
                cqSupported = encCaps.isBitrateModeSupported(
                        MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CQ);
                Log.i(TAG, "initEncoder: CQ mode supported=" + cqSupported
                        + " VBR supported="
                        + encCaps.isBitrateModeSupported(
                                MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)
                        + " CBR supported="
                        + encCaps.isBitrateModeSupported(
                                MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR));
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) { // API 31
                    Log.i(TAG, "initEncoder: CBR_FD supported="
                            + encCaps.isBitrateModeSupported(
                                    MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR_FD));
                }
                Log.i(TAG, "initEncoder: complexity range="
                        + encCaps.getComplexityRange());
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) { // API 28
                    Log.i(TAG, "initEncoder: quality range="
                            + encCaps.getQualityRange());
                }

                // --- Video capabilities ---
                Log.i(TAG, "initEncoder: supported widths="
                        + vidCaps.getSupportedWidths()
                        + " heights=" + vidCaps.getSupportedHeights());
                Log.i(TAG, "initEncoder: bitrate range="
                        + vidCaps.getBitrateRange());
                Log.i(TAG, "initEncoder: supported frame rates="
                        + vidCaps.getSupportedFrameRates());
                Log.i(TAG, "initEncoder: width alignment="
                        + vidCaps.getWidthAlignment()
                        + " height alignment=" + vidCaps.getHeightAlignment());
                if (vidCaps.isSizeSupported(mWidth, mHeight)) {
                    Log.i(TAG, "initEncoder: frame rates for "
                            + mWidth + "x" + mHeight + "="
                            + vidCaps.getSupportedFrameRatesFor(mWidth, mHeight));
                    achievableFpsRange = vidCaps.getAchievableFrameRatesFor(mWidth, mHeight);
                    Log.i(TAG, "initEncoder: achievable frame rates for "
                            + mWidth + "x" + mHeight + "="
                            + achievableFpsRange);
                } else {
                    Log.w(TAG, "initEncoder: " + mWidth + "x" + mHeight
                            + " NOT in supported size range");
                }

                // --- Color formats ---
                StringBuilder cfSb = new StringBuilder();
                for (int cf : caps.colorFormats) {
                    if (cfSb.length() > 0) cfSb.append(", ");
                    cfSb.append("0x").append(Integer.toHexString(cf));
                }
                Log.i(TAG, "initEncoder: color formats=[" + cfSb + "]");

                // --- Profile/Level pairs ---
                StringBuilder plSb = new StringBuilder();
                for (MediaCodecInfo.CodecProfileLevel pl : caps.profileLevels) {
                    if (plSb.length() > 0) plSb.append(", ");
                    plSb.append("profile=").append(pl.profile)
                         .append("/level=").append(pl.level);
                }
                Log.i(TAG, "initEncoder: profile-levels=[" + plSb + "]");

                // --- Feature support ---
                String[] features = {
                    MediaCodecInfo.CodecCapabilities.FEATURE_AdaptivePlayback,
                    MediaCodecInfo.CodecCapabilities.FEATURE_IntraRefresh,
                    MediaCodecInfo.CodecCapabilities.FEATURE_SecurePlayback,
                    MediaCodecInfo.CodecCapabilities.FEATURE_TunneledPlayback,
                };
                StringBuilder featSb = new StringBuilder();
                for (String feat : features) {
                    boolean supported = false;
                    try { supported = caps.isFeatureSupported(feat); }
                    catch (Exception ignored) {}
                    if (featSb.length() > 0) featSb.append(", ");
                    featSb.append(feat).append("=").append(supported);
                }
                Log.i(TAG, "initEncoder: features=[" + featSb + "]");
                if (!cqSupported) {
                    // VBR fallback: resolve and clamp bitrate.
                    effectiveBitRate = (mBitRate > 0)
                            ? mBitRate
                            : recommendedBitRate(mWidth, mHeight, mFrameRate,
                                                 MIME_H265.equals(mMime),
                                                 mGopMode == GopMode.ALL_INTRA);
                    int maxBr = vidCaps.getBitrateRange().getUpper();
                    Log.i(TAG, "initEncoder: bitrate range max=" + (maxBr / 1_000_000)
                            + "Mbps  requested=" + (effectiveBitRate / 1_000_000) + "Mbps"
                            + (mBitRate > 0 ? " (caller-supplied)" : " (auto)"));
                    //check requested bitrate against encoder's maximum and clamp if necessary to avoid rejections from MediaCodec.configure()
                    if (effectiveBitRate > maxBr) {
                        Log.w(TAG, "initEncoder: bitrate clamped "
                                + effectiveBitRate + " -> " + maxBr + " bps");
                        effectiveBitRate = maxBr;
                    }

                    //check requested bitrate against externally configured maximum and clamp if necessary to avoid excessive bandwidth/CPU usage
                    if (m_maxBitRate > 0 && effectiveBitRate > m_maxBitRate) {
                        Log.w(TAG, "initEncoder: bitrate capped by max_bitrate "
                                + effectiveBitRate + " -> " + m_maxBitRate + " bps");
                        effectiveBitRate = m_maxBitRate;
                    }

                    Log.i(TAG, "initEncoder: bitrate set to: " + (effectiveBitRate  / 1_000_000) + "Mbps");
                }
                break;
            }
        }

        Log.i(TAG, "initEncoder: " + encoderName + " mime=" + mMime
                + " " + mWidth + "x" + mHeight + "@" + mFrameRate + "fps"
                + " mode=" + (cqSupported ? "CQ quality=80" :
                    "VBR " + (effectiveBitRate / 1_000_000) + "Mbps"
                    + (mBitRate > 0 ? "" : " (auto)"))
                + " all-intra");

        mEncoder = MediaCodec.createByCodecName(encoderName);

        MediaFormat fmt = MediaFormat.createVideoFormat(mMime, mWidth, mHeight);

        /*
        fmt.setInteger(MediaFormat.KEY_FRAME_RATE, 25);
        fmt.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR);
        fmt.setInteger(MediaFormat.KEY_BIT_RATE, effectiveBitRate);
        fmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 0);
        fmt.setInteger(MediaFormat.KEY_VIDEO_QP_MIN, 1);
        fmt.setInteger(MediaFormat.KEY_VIDEO_QP_MAX, 4);
        fmt.setInteger(MediaFormat.KEY_VIDEO_QP_I_MIN, 1);
        fmt.setInteger(MediaFormat.KEY_VIDEO_QP_I_MAX, 3);
        fmt.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0);
        fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        fmt.setInteger("android._dataspace", 0x102);
        fmt.setInteger(MediaFormat.KEY_PRIORITY,     0); // real-time scheduling
        fmt.setFloat(MediaFormat.KEY_OPERATING_RATE, (float) 25);
        fmt.setInteger(MediaFormat.KEY_PROFILE,
            MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10);
        fmt.setInteger(MediaFormat.KEY_LEVEL,
            MediaCodecInfo.CodecProfileLevel.HEVCMainTierLevel52);
        fmt.setInteger(MediaFormat.KEY_LATENCY, 0);
        fmt.setInteger("vendor.qti-ext-enc-low-latency.enable", 1);
        fmt.setInteger("vendor.qti-ext-enc-temporal-aq.enable", 0);
*/

        //fmt.setInteger("vendor.qti-ext-enc-low-latency.enable", 1);
        //fmt.setInteger("vendor.qti-ext-enc-temporal-aq.enable", 0);

        // Clamp frame rate to achievable range reported by the encoder.
        // If mFrameRate is within the achievable range, use it as-is.
        // Otherwise use the upper bound (next lower) of the achievable range
        // to avoid requesting a rate the hardware cannot sustain.
        int effectiveFrameRate = mFrameRate;
        if (achievableFpsRange != null) {
            if (mFrameRate > achievableFpsRange.getUpper()) {
                effectiveFrameRate = (int) Math.floor(achievableFpsRange.getUpper());
                Log.w(TAG, "initEncoder: mFrameRate=" + mFrameRate
                        + " exceeds achievable upper=" + achievableFpsRange.getUpper()
                        + " → clamped to " + effectiveFrameRate + " fps");
            } else if (mFrameRate < achievableFpsRange.getLower()) {
                effectiveFrameRate = (int) Math.ceil(achievableFpsRange.getLower());
                Log.w(TAG, "initEncoder: mFrameRate=" + mFrameRate
                        + " below achievable lower=" + achievableFpsRange.getLower()
                        + " → raised to " + effectiveFrameRate + " fps");
            } else {
                Log.i(TAG, "initEncoder: mFrameRate=" + mFrameRate
                        + " within achievable range " + achievableFpsRange);
            }
        }
        fmt.setInteger(MediaFormat.KEY_FRAME_RATE,       effectiveFrameRate);
        // GOP structure: ALL_INTRA sets KEY_I_FRAME_INTERVAL=0 (every frame is an IDR);
        // SHORT_GOP sets it to mIFrameInterval seconds, enabling P-frame prediction
        // between IDRs for better quality at the same bitrate.
        // Note: in ALL_INTRA mode recommendedBitRate() uses all-intra bpp values (~2×
        // higher) to compensate for the absence of inter-frame compression.
        fmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, mIFrameInterval);
        //fmt.setInteger(MediaFormat.KEY_INTRA_REFRESH_PERIOD, 15); // Refresh 1/15th of the screen macroblocks per frame        
        Log.i(TAG, "initEncoder: GOP mode=" + mGopMode
                + (mGopMode == GopMode.SHORT_GOP
                    ? " i-frame-interval=" + mIFrameInterval + "s"
                    : " (all-intra)"));
        if (cqSupported) {
            // CQ mode: encoder ignores KEY_BIT_RATE and allocates as many bits as
            // the content requires to hit the target quality level.
            // KEY_QUALITY range: 0 (worst) – 100 (best).  80 gives excellent
            // perceptual fidelity with reasonable file sizes for broadcast use.
            fmt.setInteger(MediaFormat.KEY_BITRATE_MODE,
                    MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CQ);
            fmt.setInteger(MediaFormat.KEY_QUALITY, 80);
        } else {
            // VBR fallback: lets IDR frames use as many bits as the content requires
            // without starving subsequent P-frames.  CBR forces a fixed budget per
            // time window; after a large IDR the encoder compensates by heavily
            // quantising the next several P-frames, producing visible "breathing"
            // blocks.  VBR eliminates that artefact while still respecting the peak
            // bitrate ceiling set above.
            //fmt.setInteger(MediaFormat.KEY_BIT_RATE, effectiveBitRate);
            fmt.setInteger(MediaFormat.KEY_BITRATE_MODE,
                    MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR);
            fmt.setInteger(MediaFormat.KEY_BIT_RATE, effectiveBitRate);                       
            // QP bounds (API 31 / Android 12+): constrain how aggressively the
            // encoder can quantise frames regardless of instantaneous bitrate pressure.
            // QP scale: 0 = lossless, 51 = worst quality (H.264/H.265 standard).
            //   QP_MIN=10 : encoder never produces better-than-near-lossless output
            //               (prevents runaway bitrate spikes on static scenes).
            //   QP_MAX=30 : encoder never degrades below "broadcast quality";
            //               equivalent to ~VMAF 90+ on typical camera content.
            // Per-frame-type bounds give IDR frames extra headroom (lower max QP)
            // so keyframes are always sharper than inter-frames.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) { // API 31
                fmt.setInteger(MediaFormat.KEY_VIDEO_QP_MIN,   10);
                fmt.setInteger(MediaFormat.KEY_VIDEO_QP_MAX,   30);
                fmt.setInteger(MediaFormat.KEY_VIDEO_QP_I_MIN, 10);
                fmt.setInteger(MediaFormat.KEY_VIDEO_QP_I_MAX, 26); // IDR frames: tighter cap
                Log.i(TAG, "initEncoder: QP bounds set"
                        + " global=[10,30] IDR=[10,26]"
                        + " (API=" + Build.VERSION.SDK_INT + ")");
            } else {
                Log.i(TAG, "initEncoder: QP bounds NOT set"
                        + " (API " + Build.VERSION.SDK_INT + " < 31 – requires Android 12)");
            }
        }
        fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);

        // Bypass CCodecConfig color-aspect conversion: Camera2 tags every buffer with
        // HAL_DATASPACE_BT601_625 (legacy enum 0x102). Setting android._dataspace
        // directly prevents CCodecConfig from running colorAspectsToDataspace() which
        // would produce a mismatched new-format value, causing C2OMXNode to fire
        // onDataSpaceChanged and return C2_CORRUPTED -> MediaCodec error 0x80000000.
        fmt.setInteger("android._dataspace", 0x102);

        //if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
            fmt.setInteger(MediaFormat.KEY_LATENCY, 0);

        //if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) 
        {
            fmt.setInteger(MediaFormat.KEY_PRIORITY,     0); // real-time scheduling
            fmt.setFloat(MediaFormat.KEY_OPERATING_RATE, (float) effectiveFrameRate);
        }

        //if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
            fmt.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0);

        // Slice-based encoding (Qualcomm vendor extension).
        // Splitting each frame into multiple NAL slices allows the decoder to
        // begin reconstruction before the entire frame arrives, reducing
        // end-to-end latency.  Disabled by default (mEncSliceCount == 0);
        // enable via setEncSliceCount() before start().
        if (mEncSliceCount > 0) {
            try {
                fmt.setInteger("vendor.qti-ext-enc-slice.spacing",
                               (mWidth * mHeight * 3 / 2) / mEncSliceCount);
                Log.i(TAG, "initEncoder: set vendor.qti-ext-enc-slice.spacing for "
                        + mEncSliceCount + " slices (frame=" + mWidth + "x" + mHeight + ")");
            } catch (Exception e) {
                Log.w(TAG, "initEncoder: slice-based encoding not supported: " + e.getMessage());
            }
        }

        //fmt.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10);
        //fmt.setInteger(MediaFormat.KEY_LEVEL, MediaCodecInfo.CodecProfileLevel.HEVCMainTierLevel51);


        mEncoder.setCallback(new MediaCodec.Callback() {
            @Override
            public void onInputBufferAvailable(@NonNull MediaCodec c, int i) {
                // Not used - encoder reads from the input Surface.
            }

            @Override
            public void onOutputBufferAvailable(@NonNull MediaCodec c, int i,
                                                @NonNull MediaCodec.BufferInfo info) {
                handleEncoderOutput(c, i, info);
            }

            @Override
            public void onOutputFormatChanged(@NonNull MediaCodec c, @NonNull MediaFormat f) {
                Log.i(TAG, "Encoder output format changed: " + f);
                // video-qp-average is populated by the Qualcomm encoder in the
                // output format callback.  It reflects the actual average QP used
                // across the most recent encoded frames, giving a runtime view of
                // the quality level the encoder is operating at (lower = better).
                // Typical broadcast range: 15–28.  Values above 35 indicate the
                // encoder is bitrate-starved and degrading quality.
                if (f.containsKey("video-qp-average")) {
                    int qpAvg = f.getInteger("video-qp-average");
                    if (qpAvg == 0) {
                        // 0 is the encoder's uninitialised placeholder value;
                        // onOutputFormatChanged fires once at startup before any
                        // frame has been encoded.  Ignore it.
                        Log.d(TAG, "Encoder video-qp-average=0 (uninitialised – no frames yet)");
                    } else {
                        String quality = qpAvg <= 20 ? "excellent"
                                       : qpAvg <= 28 ? "good (broadcast)"
                                       : qpAvg <= 35 ? "acceptable"
                                       : "degraded – consider increasing bitrate";
                        Log.i(TAG, "Encoder video-qp-average=" + qpAvg + " [" + quality + "]");
                    }
                }
            }

            @Override
            public void onError(@NonNull MediaCodec c, @NonNull MediaCodec.CodecException e) {
                Log.e(TAG, "MediaCodec error: " + e.getDiagnosticInfo()
                        + " recoverable=" + e.isRecoverable()
                        + " transient=" + e.isTransient(), e);
                mRunning.set(false);
                if (!e.isRecoverable() && !e.isTransient()) {
                    int attempt = mRestartCount.incrementAndGet();
                    if (attempt <= MAX_ENCODER_RESTARTS) {
                        Log.w(TAG, "Scheduling encoder restart "
                                + attempt + "/" + MAX_ENCODER_RESTARTS);
                        new Thread(Camera2GstStreamer.this::restartEncoder,
                                "cam2gst-encoder-restart").start();
                    } else {
                        Log.e(TAG, "Restart limit reached - tearing down pipeline");
                        new Thread(Camera2GstStreamer.this::stop, "cam2gst-error-stop").start();
                    }
                } else {
                    new Thread(Camera2GstStreamer.this::stop, "cam2gst-error-stop").start();
                }
            }
        });

        Log.i(TAG, "initEncoder: configure " + fmt);
        mEncoder.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        Log.i(TAG, "initEncoder: configure complete");

        // Runtime hardware-acceleration guard: abort if the OS silently fell back
        // to a software encoder (resource exhaustion, codec alias, etc.).
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            MediaCodecInfo actual = mEncoder.getCodecInfo();
            if (!actual.isHardwareAccelerated()) {
                throw new RuntimeException("initEncoder: '" + actual.getName()
                        + "' is NOT hardware-accelerated");
            }
            Log.i(TAG, "initEncoder: HW encoder confirmed: " + actual.getName());
        }

        // android._dataspace is expected to be absent from getInputFormat():
        // CCodec strips the key after reading it. This is normal.
        MediaFormat accepted = mEncoder.getInputFormat();
        Log.i(TAG, "initEncoder: accepted input format=" + accepted);
        if (accepted.containsKey("android._dataspace")) {
            Log.d(TAG, String.format("initEncoder: accepted android._dataspace=0x%x",
                    accepted.getInteger("android._dataspace")));
        } else {
            Log.d(TAG, "initEncoder: android._dataspace not in accepted format"
                    + " - CCodec manages GraphicBufferSource dataspace internally (expected)");
        }
        // Log whether the encoder honoured the QP bounds in the accepted format.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            Log.d(TAG, "initEncoder: accepted QP_MIN="
                    + (accepted.containsKey(MediaFormat.KEY_VIDEO_QP_MIN)
                        ? accepted.getInteger(MediaFormat.KEY_VIDEO_QP_MIN) : "(not present)")
                    + " QP_MAX="
                    + (accepted.containsKey(MediaFormat.KEY_VIDEO_QP_MAX)
                        ? accepted.getInteger(MediaFormat.KEY_VIDEO_QP_MAX) : "(not present)")
                    + " QP_I_MAX="
                    + (accepted.containsKey(MediaFormat.KEY_VIDEO_QP_I_MAX)
                        ? accepted.getInteger(MediaFormat.KEY_VIDEO_QP_I_MAX) : "(not present)"));
        }

        mEncoderSurface = mEncoder.createInputSurface();
        mEncoder.start();
        Log.i(TAG, "initEncoder: started, Surface=" + mEncoderSurface);
    }

    private void handleEncoderOutput(MediaCodec codec, int index,
                                     MediaCodec.BufferInfo info) {
        try {
            if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) return;
            if (info.size == 0) return;

            ByteBuffer buf = codec.getOutputBuffer(index);
            if (buf == null) return;

            boolean isCsd      = (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0;
            boolean isKeyFrame = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME)    != 0;

            if (isCsd) {
                // Cache for re-injection when a new RTSP client connects.
                ByteBuffer copy = ByteBuffer.allocateDirect(info.size);
                int pos = buf.position(), lim = buf.limit();
                buf.position(info.offset);
                buf.limit(info.offset + info.size);
                copy.put(buf);
                copy.flip();
                buf.position(pos);
                buf.limit(lim);
                mCachedCsd = copy;
                Log.i(TAG, "handleEncoderOutput: CSD cached " + info.size + " bytes");
            } /*else if (isKeyFrame) {
                Log.d(TAG, "handleEncoderOutput: IDR " + info.size
                        + "B pts=" + info.presentationTimeUs + "us");
            }*/

            if (mRunning.get()) {
                // --- Periodic forced IDR -------------------------------------------
                // Proactively request a sync frame every FORCED_IDR_INTERVAL_FRAMES
                // P-frames (~0.5 s at 30 fps).  This caps the decoder corruption
                // window caused by a dropped P-frame (UDP loss or flow-control drop)
                // to at most half a second, independently of the encoder's natural
                // KEY_I_FRAME_INTERVAL schedule.
                if (isKeyFrame) {
                    mFramesSinceIDR.set(0);
                } else if (!isCsd) {
                    int n = mFramesSinceIDR.incrementAndGet();
                    if (n >= FORCED_IDR_INTERVAL_FRAMES) {
                        mFramesSinceIDR.set(0);
                        MediaCodec enc = mEncoder;
                        if (enc != null) {
                            Bundle p = new Bundle();
                            p.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
                            enc.setParameters(p);
                            Log.d(TAG, "handleEncoderOutput: periodic IDR requested"
                                    + " (every " + FORCED_IDR_INTERVAL_FRAMES + " frames)");
                        }
                    }
                }

                // --- Client-connect IDR gate ---------------------------------------
                if (nativeConsumeKeyframeRequest()) {
                    // Re-inject cached CSD so the parser has SPS/PPS before the IDR.
                    ByteBuffer csd = mCachedCsd;
                    if (csd != null) {
                        Log.i(TAG, "handleEncoderOutput: re-injecting CSD for new client");
                        ByteBuffer csdCopy = csd.duplicate();
                        csdCopy.rewind();
                        nativePushEncodedBuffer(csdCopy, 0, csdCopy.limit(),
                                info.presentationTimeUs, false, true);
                    } else {
                        Log.w(TAG, "handleEncoderOutput: no cached CSD - parser may drop IDR");
                    }
                    // Request an IDR immediately so the client does not wait up to
                    // KEY_I_FRAME_INTERVAL seconds for the next keyframe.
                    MediaCodec enc = mEncoder;
                    if (enc != null) {
                        Bundle params = new Bundle();
                        params.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
                        enc.setParameters(params);
                        Log.i(TAG, "handleEncoderOutput: IDR sync-frame requested for new client");
                    }
                    // Gate P-frames until the encoder delivers the requested IDR.
                    // h264parse requires CSD+IDR before it can forward P-frames;
                    // letting P-frames through first causes "broken/invalid nal" warnings.
                    mWaitingForIDR = true;
                }

                // While waiting for the IDR: skip P-frames, let CSD and IDR through.
                if (mWaitingForIDR) {
                    if (isKeyFrame) {
                        mWaitingForIDR = false; // IDR arrived – resume normal forwarding
                    } else if (!isCsd) {
                        return; // P/B frame before the IDR – withhold (releaseOutputBuffer fires in finally)
                    }
                }

                nativePushEncodedBuffer(
                        buf, info.offset, info.size,
                        info.presentationTimeUs, isKeyFrame, isCsd);
            }
        } finally {
            codec.releaseOutputBuffer(index, false);
        }
    }

    // =========================================================================
    // Step 2 - Camera2 (zero-copy: camera DMA -> encoder input Surface)
    // =========================================================================

    private void initCamera(String cameraId)
            throws CameraAccessException, InterruptedException {

        mCameraThread = new HandlerThread("Camera2GstThread");
        mCameraThread.start();
        mCameraHandler = new Handler(mCameraThread.getLooper());
        mCameraManager =
                (CameraManager) mContext.getSystemService(Context.CAMERA_SERVICE);

        if (mContext.checkSelfPermission(android.Manifest.permission.CAMERA)
                != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException(
                    "android.permission.CAMERA not granted for id=" + cameraId);
        }

        CountDownLatch latch = new CountDownLatch(1);
        mCameraManager.openCamera(cameraId, new CameraDevice.StateCallback() {
            @Override
            public void onOpened(@NonNull CameraDevice camera) {
                Log.i(TAG, "initCamera: opened id=" + cameraId);
                mCameraDevice = camera;
                latch.countDown();
            }
            @Override
            public void onDisconnected(@NonNull CameraDevice camera) {
                Log.w(TAG, "initCamera: disconnected id=" + cameraId);
                camera.close();
                mCameraDevice = null;
            }
            @Override
            public void onError(@NonNull CameraDevice camera, int error) {
                Log.e(TAG, "initCamera: error id=" + cameraId + " err=" + error);
                camera.close();
                mCameraDevice = null;
                latch.countDown();
            }
        }, mCameraHandler);

        if (!latch.await(2, TimeUnit.SECONDS) || mCameraDevice == null) {
            throw new RuntimeException("Camera open timed out (id=" + cameraId + ")");
        }
        createCaptureSession();
    }

    /**
     * Creates a TEMPLATE_RECORD repeating capture session targeting
     * {@link #mEncoderSurface}. Called from {@link #initCamera} and
     * {@link #restartEncoder}.
     */
    private void createCaptureSession()
            throws CameraAccessException, InterruptedException {

        CaptureRequest.Builder builder =
                mCameraDevice.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
        builder.addTarget(mEncoderSurface);
        builder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE,
                new Range<>(mFrameRate, mFrameRate));
        builder.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE,
                CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF);

        List<OutputConfiguration> outputs = new ArrayList<>();
        outputs.add(new OutputConfiguration(mEncoderSurface));

        CountDownLatch latch = new CountDownLatch(1);
        mCameraDevice.createCaptureSession(new SessionConfiguration(
                SessionConfiguration.SESSION_REGULAR,
                outputs,
                cmd -> mCameraHandler.post(cmd),
                new CameraCaptureSession.StateCallback() {
                    @Override
                    public void onConfigured(@NonNull CameraCaptureSession session) {
                        mCaptureSession = session;
                        try {
                            session.setRepeatingRequest(builder.build(), null, mCameraHandler);
                        } catch (CameraAccessException e) {
                            Log.e(TAG, "setRepeatingRequest: " + e.getMessage(), e);
                        }
                        latch.countDown();
                    }
                    @Override
                    public void onConfigureFailed(@NonNull CameraCaptureSession session) {
                        Log.e(TAG, "createCaptureSession: configure failed");
                        latch.countDown();
                    }
                }));

        if (!latch.await(2, TimeUnit.SECONDS) || mCaptureSession == null) {
            throw new RuntimeException("Camera session creation timed out");
        }
        Log.i(TAG, "createCaptureSession: camera " + mCameraId
                + " streaming -> encoder Surface (zero-copy DMA path)");
    }

    // =========================================================================
    // Step 3 - GStreamer pipeline
    // =========================================================================

    /**
     * AAC AudioSpecificConfig sampling-frequency index for {@code rate}, or
     * {@code -1} if the rate is not an AAC-supported rate (ISO/IEC 14496-3
     * Table 1.16).  Also used by {@link #setAudioSampleRate(int)} to validate.
     */
    private static int aacFreqIndex(int rate) {
        switch (rate) {
            case 96000: return 0;
            case 88200: return 1;
            case 64000: return 2;
            case 48000: return 3;
            case 44100: return 4;
            case 32000: return 5;
            case 24000: return 6;
            case 22050: return 7;
            case 16000: return 8;
            case 12000: return 9;
            case 11025: return 10;
            case 8000:  return 11;
            default:    return -1;
        }
    }

    /**
     * Builds the GStreamer appsrc caps string for the raw AAC-LC audio branch
     * from the current {@link #AUDIO_SAMPLE_RATE} / {@link #AUDIO_CHANNEL_COUNT},
     * computing the 2-byte AudioSpecificConfig (codec_data) so it always matches
     * the rate/channels.  aacparse needs codec_data for stream-format=raw (else
     * "Need codec_data for raw AAC" / not-negotiated).
     *
     * ASC bit layout (16 bits): objectType=2 (5b) | freqIdx (4b) | channels (4b)
     * | 0 (3b).  e.g. 44100/mono=0x1208, 48000/stereo=0x1190, 48000/mono=0x1188.
     */
    private static String buildAacAudioCaps() {
        int asc16 = (2 << 11) | (aacFreqIndex(AUDIO_SAMPLE_RATE) << 7)
                  | (AUDIO_CHANNEL_COUNT << 3);
        String codecData = String.format("%04x", asc16);
        return "audio/mpeg,mpegversion=(int)4,stream-format=(string)raw"
             + ",channels=(int)" + AUDIO_CHANNEL_COUNT
             + ",rate=(int)" + AUDIO_SAMPLE_RATE
             + ",codec_data=(buffer)" + codecData;
    }

    /**
     * Builds the GStreamer appsrc caps string for the raw-PCM (Opus) audio
     * branch from the current {@link #AUDIO_SAMPLE_RATE} /
     * {@link #AUDIO_CHANNEL_COUNT}.  No codec_data is needed because the appsrc
     * carries uncompressed PCM that {@code opusenc} encodes downstream.  The
     * {@code rate=(int)} / {@code channels=(int)} tokens let the native side
     * detect the format the same way it does for AAC.
     */
    private static String buildPcmAudioCaps() {
        return "audio/x-raw,format=(string)S16LE,layout=(string)interleaved"
             + ",channels=(int)" + AUDIO_CHANNEL_COUNT
             + ",rate=(int)" + AUDIO_SAMPLE_RATE;
    }

    /**
     * Builds the Opus audio sub-pipeline: raw-PCM appsrc → audioconvert →
     * opusenc → rtpopuspay.  {@code do-timestamp=false} so the explicit
     * capture-start PTS computed in the audio thread is honoured (matches the
     * AAC branch).  {@code frame-size}={@link #OPUS_FRAME_MS} keeps encoder
     * latency low.  {@code audio-type=voice} biases the encoder for speech;
     * change to {@code generic} for music.  The queue is non-leaky (small audio
     * frames, CBR) to avoid dropping samples.
     *
     * @param payName the RTP payloader element name (e.g. {@code pay0} for
     *                audio-only, {@code pay1} when multiplexed with video).
     */
    private static String buildOpusAudioBranch(String payName) {
        return "appsrc name=audio is-live=true format=time do-timestamp=false"
             + " caps=" + buildPcmAudioCaps()
             + " ! queue name=audPostQ max-size-buffers=8 max-size-bytes=0"
                 + " max-size-time=0"
             + " ! audioconvert"
             + " ! opusenc name=aenc bitrate=" + OPUS_BITRATE
                 + " frame-size=" + OPUS_FRAME_MS
                 + " audio-type=voice"
             + " ! rtpopuspay name=" + payName + " pt=" + OPUS_RTP_PAYLOAD_TYPE;
    }

    private void initGstPipeline() {
        String pipeline;
        
        // Override with a custom pipeline if one was set via setCustomPipeline().
        if (!m_customePipeline.isEmpty()) {
            // Custom pipeline override: bypass the default builder entirely.
            pipeline = m_customePipeline;
            Log.i(TAG, "initGstPipeline: using custom pipeline override");
        } else if (mStreamMode == StreamMode.AUDIO_ONLY) {
            // Audio-only: single audio branch as pay0.  Opus (raw PCM → opusenc →
            // rtpopuspay) when m_useOpus, else AAC (appsrc → aacparse →
            // rtpmp4apay).  caps are built from AUDIO_SAMPLE_RATE /
            // AUDIO_CHANNEL_COUNT so they always match what is captured.
            if (m_useOpus) {
                pipeline = "( " + buildOpusAudioBranch("pay0") + " )";
            } else {
                pipeline = "( appsrc name=audio is-live=true format=time do-timestamp=true"
                    + " caps=" + buildAacAudioCaps()
                    + " ! queue name=audPostQ max-size-buffers=8 max-size-bytes=0"
                        + " max-size-time=0"
                    + " ! aacparse ! rtpmp4apay name=pay0 pt=97 )";
            }
        } else {
            boolean h265      = MIME_H265.equals(mMime);
            String codecShort = h265 ? "h265" : "h264";

            // aggregate-mode=zero-latency: one RTP packet per NAL unit, no batching delay.
            // config-interval=-1: embed VPS/SPS/PPS in-band before every IDR.
            String payloader = (h265 ? "rtph265pay" : "rtph264pay")
                    + " config-interval=-1 aggregate-mode=zero-latency name=pay0 pt=96";

            // h26x_parse_enable=false (default): omit the parser for lowest latency.
            // h26x_parse_enable=true: insert h264parse/h265parse for more robust caps
            // negotiation (adds ~1 frame of buffering latency).
            String parseStep = mH26xParseEnable
                    ? " ! " + (h265 ? "h265parse" : "h264parse")
                    : "";
            String preQueue = "queue max-size-buffers=2 max-size-time=0 max-size-bytes=0 leaky=downstream";
            
            // vidPostQ removed: appsrc already has an internal buffer; an extra queue
            // element adds a GLib thread-boundary context switch (~0-33 ms scheduling
            // jitter) for zero benefit on a live push-mode source.
            String videoBranch = " appsrc name=src is-live=true format=time do-timestamp=true"
                + " caps=video/x-" + codecShort + ",stream-format=byte-stream,alignment=au"
                + parseStep 
                //+ " ! " + preQueue  -- just not to use any queue for now.
                + " ! " + payloader;

            if (mStreamMode == StreamMode.VIDEO_ONLY) {
                // Video-only: no audio appsrc/pay1 branch.
                pipeline = "(" + videoBranch + " )";
            } else {
                // VIDEO_AND_AUDIO_BOTH (default): video pay0 + audio pay1.
                // Audio is Opus (raw PCM → opusenc → rtpopuspay) when m_useOpus,
                // else AAC (appsrc → aacparse → rtpmp4apay).  caps are built from
                // AUDIO_SAMPLE_RATE / AUDIO_CHANNEL_COUNT so they always match.
                if (m_useOpus) {
                    pipeline = "(" + videoBranch
                        + "  " + buildOpusAudioBranch("pay1") + " )";
                } else {
                    pipeline = "(" + videoBranch
                        + "  appsrc name=audio is-live=true format=time do-timestamp=true"
                        + " caps=" + buildAacAudioCaps()
                        + " ! queue name=audPostQ max-size-buffers=8 max-size-bytes=0"
                            + " max-size-time=0"
                        + " ! aacparse ! rtpmp4apay name=pay1 pt=97 )";
                }
            }
        }

        Log.i(TAG, "initGstPipeline [" + mStreamMode + "] transport=" + mTransportMode
                + " cpuAffinity=" + mCpuAffinityEnable
                + " tcpNoDelay=" + mTcpNoDelay
                + " sndbuf=" + mSoSndbufSize
                + " notsentLowat=" + mTcpNotsentLowat
                + " ipTosDscpEf=" + mIpTosDscpEf
                + " h26xParse=" + mH26xParseEnable
                + " rtpbinBufModeNone=" + mRtpbinBufferModeNone
                + " periodicDump=" + mPeriodicDumpEnable
                + ": " + pipeline);
        // Push feature flags to C++ before pipeline init so they are visible
        // to the GMainLoop thread and any callbacks that fire during init.
        nativeSetCpuAffinityEnable(mCpuAffinityEnable);
        nativeSetTcpNoDelay(mTcpNoDelay);
        nativeSetSoSndbufSize(mSoSndbufSize);
        nativeSetTcpNotsentLowat(mTcpNotsentLowat);
        nativeSetIpTosDscpEf(mIpTosDscpEf);
        nativeSetH26xParseEnable(mH26xParseEnable);
        nativeSetRtpbinBufferModeNone(mRtpbinBufferModeNone);
        nativeSetPeriodicDumpEnable(mPeriodicDumpEnable);
        nativeSetRtcpFeedback(m_rtcpFeedback);
        if (!nativeGstPipelineInit(pipeline, mPort,
                mTransportMode == TransportMode.UDP ? 1 : 0)) {
            throw new RuntimeException("GStreamer RTSP server init failed");
        }
        mGstPipelineStarted = true;
        Log.i(TAG, "initGstPipeline: RTSP server started on port " + mPort);
    }

    // =========================================================================
    // Teardown helpers
    // =========================================================================

    private void releaseCamera() {
        if (mCaptureSession != null) {
            try { mCaptureSession.stopRepeating(); } catch (Exception ignored) {}
            mCaptureSession.close();
            mCaptureSession = null;
        }
        if (mCameraDevice != null) {
            mCameraDevice.close();
            mCameraDevice = null;
        }
        if (mCameraThread != null) {
            mCameraThread.quitSafely();
            mCameraThread = null;
        }
        Log.i(TAG, "releaseCamera: done");
    }

    private void releaseEncoder() {
        if (mEncoder != null) {
            try { mEncoder.stop(); } catch (Exception ignored) {}
            mEncoder.release();
            mEncoder = null;
        }
        if (mEncoderSurface != null) {
            mEncoderSurface.release();
            mEncoderSurface = null;
        }
        mCachedCsd = null;
        mWaitingForIDR = false;
        mFramesSinceIDR.set(0);
        Log.i(TAG, "releaseEncoder: done");
    }

    // =========================================================================
    // Audio capture – AudioRecord → MediaCodec AAC HW encoder (Qualcomm QCS8250)
    // =========================================================================

    private void startAudioCapture() {
        if (mContext.checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)
                != PackageManager.PERMISSION_GRANTED) {
            Log.w(TAG, "startAudioCapture: RECORD_AUDIO permission not granted – audio disabled");
            return;
        }

        // ---- AudioRecord setup ------------------------------------------------
        final int channelMask = (AUDIO_CHANNEL_COUNT == 2)
                ? AudioFormat.CHANNEL_IN_STEREO
                : AudioFormat.CHANNEL_IN_MONO;
        int minBuf = AudioRecord.getMinBufferSize(
                AUDIO_SAMPLE_RATE,
                channelMask,
                AudioFormat.ENCODING_PCM_16BIT);
        if (minBuf == AudioRecord.ERROR || minBuf == AudioRecord.ERROR_BAD_VALUE) {
            Log.e(TAG, "startAudioCapture: AudioRecord.getMinBufferSize failed");
            return;
        }

        // NOTE: Do NOT change the audio source away from CAMCORDER. We tried
        // VOICE_RECOGNITION / UNPROCESSED to chase lower capture latency and the
        // CLIENT GOT NO AUDIO AT ALL (verified 2026-06-24, QCS8250). CAMCORDER is
        // the only source that produces a working stream on this device. See
        // AUDIO_CAPTURE_NOTES.md. This AudioRecord is shared by BOTH the AAC and
        // Opus paths, so the source applies to both codecs.
        int recBufSize = Math.max(minBuf * 4, 8192);
        try {
            mAudioRecord = new AudioRecord(
                    MediaRecorder.AudioSource.CAMCORDER,
                    AUDIO_SAMPLE_RATE,
                    channelMask,
                    AudioFormat.ENCODING_PCM_16BIT,
                    recBufSize);
            Log.i(TAG, "startAudioCapture: AudioRecord source=CAMCORDER"
                    + " bufBytes=" + recBufSize + " (minBuf=" + minBuf + ")");
            if (mAudioRecord.getState() != AudioRecord.STATE_INITIALIZED) {
                Log.e(TAG, "startAudioCapture: AudioRecord not initialized");
                mAudioRecord.release();
                mAudioRecord = null;
                return;
            }
            // getBufferSizeInFrames() reports the CAPACITY of the client ring
            // buffer (here it just echoes the recBufSize we requested) — it is
            // NOT the standing latency, and NOT a fast/slow-path indicator.
            // Actual capture delay = how full the ring is when read() returns,
            // which our 10/20 ms read loops keep near one HAL period (~60 ms
            // floor on this device). See AUDIO_CAPTURE_NOTES.md.
            Log.i(TAG, "startAudioCapture: AudioRecord bufFrames="
                    + mAudioRecord.getBufferSizeInFrames()
                    + " (ring CAPACITY, not delay)");
        } catch (Exception e) {
            Log.e(TAG, "startAudioCapture: AudioRecord init failed: " + e.getMessage(), e);
            releaseAudio();
            return;
        }

        // ---- MediaCodec AAC encoder setup ------------------------------------
        // Try named Qualcomm HW encoder first, fall back to any HW encoder,
        // then software.  nativePushAudioBuffer() expects raw AAC-LC access
        // units (no ADTS header) which is what MediaCodec produces for MIME
        // audio/mp4a-latm.  BUFFER_FLAG_CODEC_CONFIG frames are skipped here;
        // the profile/rate/channels are already present in the GStreamer caps.
        //
        // Skipped entirely on the Opus path (m_useOpus): Opus encoding happens
        // in the GStreamer opusenc element, so no MediaCodec encoder is created
        // and mAudioEncoder stays null.
        if (!m_useOpus) {
        try {
            String encName = findAudioHardwareEncoder();
            if (encName != null) {
                mAudioEncoder = MediaCodec.createByCodecName(encName);
                Log.i(TAG, "startAudioCapture: using AAC encoder: " + encName);
            } else {
                mAudioEncoder = MediaCodec.createEncoderByType(MIME_AAC);
                Log.i(TAG, "startAudioCapture: using system-default AAC encoder: "
                        + mAudioEncoder.getCodecInfo().getName());
            }
            MediaFormat fmt = MediaFormat.createAudioFormat(
                    MIME_AAC, AUDIO_SAMPLE_RATE, AUDIO_CHANNEL_COUNT);
            fmt.setInteger(MediaFormat.KEY_BIT_RATE, AUDIO_AAC_BITRATE);
            fmt.setInteger(MediaFormat.KEY_AAC_PROFILE,
                    MediaCodecInfo.CodecProfileLevel.AACObjectLC);
            mAudioEncoder.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            mAudioEncoder.start();
        } catch (Exception e) {
            Log.e(TAG, "startAudioCapture: AAC encoder init failed: " + e.getMessage(), e);
            releaseAudio();
            return;
        }
        }  // end if (!m_useOpus)

        mAudioRunning = true;
        mAudioRecord.startRecording();

        // ---- Opus path: push raw PCM straight to GStreamer (opusenc) ----------
        if (m_useOpus) {
            mAudioThread = new Thread(this::runOpusPushLoop, "cam2gst-audio");
            mAudioThread.setDaemon(true);
            mAudioThread.start();
            Log.i(TAG, "startAudioCapture: Opus audio thread started (GStreamer opusenc, "
                    + AUDIO_SAMPLE_RATE + " Hz "
                    + (AUDIO_CHANNEL_COUNT == 2 ? "stereo" : "mono") + " "
                    + OPUS_BITRATE / 1000 + " kbps, frame=" + OPUS_FRAME_MS + " ms)");
            return;
        }


        // 20 ms of PCM = (rate * 0.020) frames * bytesPerSample * channels.
        // e.g. 44100 mono 16-bit = 1764 B; 48000 stereo 16-bit = 3840 B per read.
        final int pcmFrameBytes = (int)(AUDIO_SAMPLE_RATE * 0.020f)
                                * AUDIO_PCM_FRAME_BYTES * AUDIO_CHANNEL_COUNT;
        final byte[]             pcmBuf  = new byte[pcmFrameBytes];
        final MediaCodec.BufferInfo outInfo = new MediaCodec.BufferInfo();
        // Pre-allocated direct buffer for AAC output (one AAC-LC frame ≤ 2 KB).
        final ByteBuffer aacOut = ByteBuffer.allocateDirect(4096);

        mAudioThread = new Thread(() -> {
            while (mAudioRunning) {
                // ---- 1. Feed PCM into encoder input buffer ----
                int inputIdx = mAudioEncoder.dequeueInputBuffer(10_000 /*10 ms timeout*/);
                if (inputIdx >= 0) {
                    int bytesRead = mAudioRecord.read(pcmBuf, 0, pcmFrameBytes);
                    if (bytesRead > 0) {
                        ByteBuffer inBuf = mAudioEncoder.getInputBuffer(inputIdx);
                        if (inBuf != null) {
                            inBuf.clear();
                            inBuf.put(pcmBuf, 0, bytesRead);
                        }
                        // Use capture-START time, not capture-END time, as the PTS.
                        // mAudioRecord.read() blocks until bytesRead bytes are in the
                        // hardware ring buffer; at the moment it returns the LAST sample
                        // was just captured.  The FIRST sample was captured
                        // (bytesRead / bytes_per_sample / sampleRate) seconds earlier.
                        // Subtracting that duration gives the true capture-start PTS.
                        //
                        // CLOCK MATCH (verified June 2026 on TSx camera id=0):
                        // elapsedRealtimeNanos() is CLOCK_BOOTTIME.  We read
                        // SENSOR_INFO_TIMESTAMP_SOURCE in listCameras() and the device
                        // returned 1 = REALTIME, which means the Camera2 sensor ALSO
                        // stamps its buffers on CLOCK_BOOTTIME (the elapsedRealtimeNanos
                        // base).  So audio and video share the SAME clock here — there is
                        // no cross-clock skew.
                        //
                        // DO NOT change this to System.nanoTime(): that is CLOCK_MONOTONIC
                        // and would DESYNC audio from the BOOTTIME camera clock.  (The two
                        // only coincide while the device never suspends — ≈0 on always-on
                        // TSx — but matching the camera's actual clock is the correct
                        // principle.)  If a FUTURE product instead reports
                        // SENSOR_INFO_TIMESTAMP_SOURCE=0 (UNKNOWN/MONOTONIC), THEN switch
                        // this to System.nanoTime() to match that device.  Decide from the
                        // logged timestamp source, not by assumption.  See ARCHITECTURE.md
                        // "Known Issues – A/V Sync" Issue 1.
                        long captureEndUs   = android.os.SystemClock.elapsedRealtimeNanos() / 1000L;
                        long bufDurationUs  = (long) bytesRead * 1_000_000L
                                             / (AUDIO_SAMPLE_RATE * AUDIO_PCM_FRAME_BYTES
                                                * AUDIO_CHANNEL_COUNT);
                        long ptsUs = captureEndUs - bufDurationUs; // capture-start time
                        mAudioEncoder.queueInputBuffer(inputIdx, 0, bytesRead, ptsUs, 0);
/*******Measure the aac encoder pipeline latency************************************************************************/
                        // [AAC-LATENCY] remember when this PCM entered the encoder,
                        // keyed by its PTS, so the output drain can measure the
                        // encoder's queue->emit latency.  Cap the map so a stalled
                        // encoder can never leak it.
                        ////mAacInWallUs.put(ptsUs,
                        ////        android.os.SystemClock.elapsedRealtimeNanos() / 1000L);
                        ////if (mAacInWallUs.size() > 16) mAacInWallUs.pollFirstEntry();
/*******Measure the encoder pipeline latency************************************************************************/
                    } else {
                        // AudioRecord not ready – re-queue empty buffer.
                        mAudioEncoder.queueInputBuffer(inputIdx, 0, 0, 0, 0);
                    }
                }

                // ---- 2. Drain encoded AAC output ----
                int outputIdx;
                while ((outputIdx = mAudioEncoder.dequeueOutputBuffer(outInfo, 0)) >= 0) {
                    boolean isCfg = (outInfo.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0;
                    if (!isCfg && outInfo.size > 0) {
/*******Measure the aac encoder pipeline latency************************************************************************/
                        // [AAC-LATENCY] Measure the encoder pipeline latency for this
                        // frame.  The output PTS is interpolated to the first sample
                        // in the AAC frame, which generally falls INSIDE one of the
                        // 20 ms input chunks (1024-sample AAC frames don't align to
                        // 20 ms PCM reads), so floor-match it to the input chunk that
                        // sample came from.  Both timestamps are CLOCK_BOOTTIME.
                        //// long nowUs = android.os.SystemClock.elapsedRealtimeNanos() / 1000L;
                        //// java.util.Map.Entry<Long,Long> in =
                        ////         mAacInWallUs.floorEntry(outInfo.presentationTimeUs);
                        //// if (in != null && (++mAacLatCount % 50) == 0) {
                        ////     long encLatMs  = (nowUs - in.getValue()) / 1000L;          // PCM queue -> AAC emit
                        ////     long capEmitMs = (nowUs - outInfo.presentationTimeUs) / 1000L; // capture-start -> AAC emit
                        ////     Log.i(TAG, String.format(java.util.Locale.US,
                        ////             "AAC latency [%s]: encoder=%d ms (PCM queue->AAC emit)  "
                        ////           + "capture->emit=%d ms  (in-flight=%d chunks)",
                        ////             mAudioEncoder.getCodecInfo().getName(),
                        ////             encLatMs, capEmitMs, mAacInWallUs.size()));
                        //// }
/*******Measure the encoder pipeline latency************************************************************************/

                        ByteBuffer outBuf = mAudioEncoder.getOutputBuffer(outputIdx);
                        if (outBuf != null) {
                            outBuf.position(outInfo.offset);
                            outBuf.limit(outInfo.offset + outInfo.size);
                            aacOut.clear();
                            aacOut.put(outBuf);
                            aacOut.flip();
                            nativePushAudioBuffer(aacOut, 0, outInfo.size,
                                    outInfo.presentationTimeUs);
                        }
                    }
                    mAudioEncoder.releaseOutputBuffer(outputIdx, false);
                    if ((outInfo.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break;
                }
                // dequeueOutputBuffer < 0: INFO_OUTPUT_BUFFERS_CHANGED,
                // INFO_OUTPUT_FORMAT_CHANGED, or INFO_TRY_AGAIN_LATER – all normal.
            }
            Log.i(TAG, "audioThread: exiting");
        }, "cam2gst-audio");
        mAudioThread.setDaemon(true);
        mAudioThread.start();
        String aacName = mAudioEncoder.getCodecInfo().getName();
        boolean aacHw  = (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
                       && mAudioEncoder.getCodecInfo().isHardwareAccelerated();
        Log.i(TAG, "startAudioCapture: AAC encoder thread started: " + aacName
                + " (" + (aacHw ? "hardware" : "software") + ", "
                + AUDIO_SAMPLE_RATE + " Hz "
                + (AUDIO_CHANNEL_COUNT == 2 ? "stereo" : "mono") + " "
                + AUDIO_AAC_BITRATE / 1000 + " kbps)");
    }

    /**
     * Opus capture loop: read one {@link #OPUS_FRAME_MS} chunk of PCM from
     * {@link #mAudioRecord} per pass and push it (raw, uncompressed) to the
     * native audio appsrc, where the GStreamer {@code opusenc} element encodes
     * it.  Mirrors the AAC loop's capture-start PTS computation so A/V sync
     * behaviour is identical.  Runs on the audio thread until
     * {@link #mAudioRunning} clears.  Used only when {@link #m_useOpus} is set;
     * {@link #mAudioEncoder} stays null on this path.
     *
     * NOTE: the "Opus push latency" logged here is capture-start → appsrc-push
     * (the Java-side cost only).  The opusenc compute happens downstream inside
     * GStreamer and is NOT included, so this number is NOT directly comparable
     * to the AAC "capture->emit" figure (which includes the MediaCodec encode).
     */
    private void runOpusPushLoop() {
        // One Opus frame of PCM, e.g. 10 ms @ 48000 Hz mono 16-bit = 960 B.
        final int pcmFrameBytes = (int)(AUDIO_SAMPLE_RATE * (OPUS_FRAME_MS / 1000f))
                                * AUDIO_PCM_FRAME_BYTES * AUDIO_CHANNEL_COUNT;
        final byte[]     pcmBuf = new byte[pcmFrameBytes];
        final ByteBuffer pcmOut = ByteBuffer.allocateDirect(pcmFrameBytes);

        long pushCount = 0, latMinUs = Long.MAX_VALUE, latMaxUs = 0, latSumUs = 0;

        while (mAudioRunning) {
            int bytesRead = mAudioRecord.read(pcmBuf, 0, pcmFrameBytes);
            if (bytesRead <= 0) continue;

            // Capture-START PTS — identical reasoning to the AAC loop: read()
            // returns once the LAST sample is in the ring buffer, so subtract the
            // chunk duration to recover the first-sample time.  CLOCK_BOOTTIME,
            // shared with the camera clock (see the AAC loop comment above).
            long captureEndUs  = android.os.SystemClock.elapsedRealtimeNanos() / 1000L;
            long bufDurationUs  = (long) bytesRead * 1_000_000L
                                 / (AUDIO_SAMPLE_RATE * AUDIO_PCM_FRAME_BYTES
                                    * AUDIO_CHANNEL_COUNT);
            long ptsUs = captureEndUs - bufDurationUs; // capture-start time

            pcmOut.clear();
            pcmOut.put(pcmBuf, 0, bytesRead);
            pcmOut.flip();
            nativePushAudioBuffer(pcmOut, 0, bytesRead, ptsUs);

            // Opus push latency: capture-start -> appsrc push (Java side only).
            long pushUs = android.os.SystemClock.elapsedRealtimeNanos() / 1000L;
            long latUs  = pushUs - ptsUs;
            if (latUs < latMinUs) latMinUs = latUs;
            if (latUs > latMaxUs) latMaxUs = latUs;
            latSumUs += latUs;
            if ((++pushCount % 100) == 0) {
                if (m_audioLatencyDebug) {
                    Log.i(TAG, String.format(java.util.Locale.US,
                            "Opus push latency: min=%d ms avg=%d ms max=%d ms "
                          + "(capture-start->appsrc; opusenc compute not included)",
                            latMinUs / 1000L, (latSumUs / 100L) / 1000L, latMaxUs / 1000L));
                }
                latMinUs = Long.MAX_VALUE; latMaxUs = 0; latSumUs = 0;
            }
        }
        Log.i(TAG, "audioThread: exiting (opus)");
    }

    private void stopAudioCapture() {
        mAudioRunning = false;
        if (mAudioThread != null) {
            try { mAudioThread.join(2000); } catch (InterruptedException ignored) {}
            mAudioThread = null;
        }
        releaseAudio();
    }

    private void releaseAudio() {
        if (mAudioEncoder != null) {
            try { mAudioEncoder.stop(); } catch (Exception ignored) {}
            mAudioEncoder.release();
            mAudioEncoder = null;
        }
        if (mAudioRecord != null) {
            try { mAudioRecord.stop(); } catch (Exception ignored) {}
            mAudioRecord.release();
            mAudioRecord = null;
        }
        Log.i(TAG, "releaseAudio: done");
    }

    // =========================================================================
    // Bitrate helper
    // =========================================================================

    /**
     * Calculates a recommended VBR peak bitrate.
     *
     * <p>All-intra requires ~2× the bitrate of IBP for equivalent quality because
     * there is no inter-frame prediction; every frame is encoded independently.
     *
     * <p>Bits-per-pixel (bpp) model:
     * <pre>
     *   H.264 all-intra : 0.20 bpp
     *   H.265 all-intra : 0.10 bpp  (HEVC intra ~50 % more efficient than AVC)
     *   H.264 short-GOP : 0.10 bpp
     *   H.265 short-GOP : 0.05 bpp
     * </pre>
     *
     * Representative results (all-intra / short-GOP):
     * <pre>
     *   4K   (3840×2160) 30fps  H.264 → 50 / 25 Mbps   H.265 → 25 / 12.5 Mbps
     *   1080p(1920×1080) 30fps  H.264 → 12.5 / 6 Mbps  H.265 →  6 /  3 Mbps
     *   720p (1280× 720) 30fps  H.264 →  5.5 / 2.8 Mbps H.265 → 2.5 / 1.4 Mbps
     * </pre>
     *
     * Rounded to the nearest 500 Kbps and capped at
     * 50 Mbps (H.264) / 25 Mbps (H.265).
     */
    private static int recommendedBitRate(int width, int height, int frameRate,
                                          boolean h265, boolean allIntra) {
        // bpp: all-intra needs 2× vs IBP because no inter-frame prediction is used.
        double bpp     = h265 ? (allIntra ? 0.10 : 0.05)
                              : (allIntra ? 0.20 : 0.10);
        long   raw     = (long)(width * (long)height * frameRate * bpp);
        long   rounded = ((raw + 250_000L) / 500_000L) * 500_000L;
        int    maxBps  = h265 ? 25_000_000 : 50_000_000;
        return (int) Math.min(rounded, maxBps);
    }

    // =========================================================================
    // Encoder discovery
    // =========================================================================

    /**
     * Returns the name of the best available hardware encoder and sets
     * {@link #mMime}. The codec selected by {@link #m_preferH265} is tried first
     * (named Qualcomm variants, then any HW encoder); the other codec is used
     * as a fallback if the preferred one is unavailable.
     */
    private String findHardwareEncoder() {
        String mime1   = m_preferH265 ? MIME_H265 : MIME_H264;
        String mime2   = m_preferH265 ? MIME_H264 : MIME_H265;
        String[] pref  = m_preferH265 ? HEVC_HW_ENCODERS : AVC_HW_ENCODERS;
        String[] fallb = m_preferH265 ? AVC_HW_ENCODERS  : HEVC_HW_ENCODERS;

        // 1. Named Qualcomm encoders for preferred codec.
        for (String name : pref) {
            if (isEncoderPresent(name)) {
                Log.i(TAG, "findHardwareEncoder: " + name + " (" + mime1 + ")");
                mMime = mime1;
                return name;
            }
        }
        // 2. Any HW encoder for preferred codec.
        String gen = findHardwareEncoderForMime(mime1);
        if (gen != null) {
            Log.i(TAG, "findHardwareEncoder: " + gen + " (" + mime1 + ")");
            mMime = mime1;
            return gen;
        }

        Log.w(TAG, "findHardwareEncoder: no " + mime1 + " encoder - trying " + mime2);

        // 3. Named Qualcomm encoders for fallback codec.
        for (String name : fallb) {
            if (isEncoderPresent(name)) {
                Log.i(TAG, "findHardwareEncoder (fallback): " + name + " (" + mime2 + ")");
                mMime = mime2;
                return name;
            }
        }
        // 4. Any HW encoder for fallback codec.
        gen = findHardwareEncoderForMime(mime2);
        if (gen != null) {
            Log.i(TAG, "findHardwareEncoder (fallback): " + gen + " (" + mime2 + ")");
            mMime = mime2;
            return gen;
        }

        Log.e(TAG, "findHardwareEncoder: no hardware encoder found");
        return null;
    }

    private static boolean isEncoderPresent(String name) {
        for (MediaCodecInfo info :
                new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (info.isEncoder() && info.getName().equalsIgnoreCase(name)) {
                // Do not gate on isHardwareAccelerated() for named vendor encoders:
                // OMX.qcom.* wrapped via CCodec shim returns false on API 31+ even
                // though the underlying implementation is fully hardware.
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    Log.d(TAG, "isEncoderPresent: " + name
                            + " hwAccel=" + info.isHardwareAccelerated()
                            + " (trusting vendor name)");
                }
                return true;
            }
        }
        return false;
    }

    private static String findHardwareEncoderForMime(String mime) {
        for (MediaCodecInfo info :
                new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (!info.isEncoder()) continue;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                if (!info.isHardwareAccelerated()) continue;
            } else {
                // API < 29: filter known AOSP software encoders by name.
                String ln = info.getName().toLowerCase();
                if (ln.startsWith("omx.google.") || ln.startsWith("c2.android.")) continue;
            }
            for (String t : info.getSupportedTypes()) {
                if (mime.equalsIgnoreCase(t)) return info.getName();
            }
        }
        return null;
    }

    /**
     * Returns the name of the best available hardware AAC encoder.
     * Tries named Qualcomm vendor encoders first, then any hardware AAC encoder.
     * Returns null if no hardware encoder is found (caller should use
     * MediaCodec.createEncoderByType() as software fallback).
     */
    private String findAudioHardwareEncoder() {
        // 1. Named Qualcomm AAC hardware encoders, preferred first.
        for (String name : AAC_HW_ENCODERS) {
            if (isEncoderPresent(name)) {
                Log.i(TAG, "findAudioHardwareEncoder: " + name);
                return name;
            }
        }
        // 2. Any hardware AAC encoder reported by the platform.
        String gen = findHardwareEncoderForMime(MIME_AAC);
        if (gen != null) {
            Log.i(TAG, "findAudioHardwareEncoder: generic HW encoder: " + gen);
            return gen;
        }
        // 3. No hardware encoder found; caller will use createEncoderByType (software).
        Log.w(TAG, "findAudioHardwareEncoder: no hardware AAC encoder found – software fallback");
        return null;
    }
}
