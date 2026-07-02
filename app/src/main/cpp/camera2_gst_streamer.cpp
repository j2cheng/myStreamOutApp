/**
 * Copyright (C) 2024 to the present, Crestron Electronics, Inc.
 * All rights reserved.
 *
 * \file        camera2_gst_streamer.cpp
 *
 * \brief       JNI bridge for Camera2GstStreamer.java.
 *
 *              Creates a GstRTSPServer that serves:
 *                  appsrc → h265parse/h264parse → rtph265pay/rtph264pay (pay0)
 *                  appsrc → aacparse → rtpmp4apay (pay1)  [audio]
 *              as an RTSP stream on the configured port.
 *
 *              Java pushes MediaCodec-encoded Annex-B AU buffers via
 *              nativePushEncodedBuffer(), and AAC-LC ADTS frames via
 *              nativePushAudioBuffer(); this module feeds them into the
 *              respective GStreamer appsrc elements with accurate PTS values.
 *
 * \note        GStreamer must already be initialised (via GstreamBase) before
 *              nativeGstPipelineInit() is called.
 */

#include <jni.h>
#include <android/log.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/sdp/sdp.h>   // GstSDPMessage / GstSDPMedia – setup_sdp override
#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include <map>              // std::map (opusenc encode-latency probe)
#include <stdio.h>          // fopen, fscanf, fwrite
#include <errno.h>
#include <time.h>           // time, localtime, strftime (DOT file timestamps)
#include <sched.h>          // sched_setaffinity, cpu_set_t
#include <unistd.h>         // sysconf, _SC_NPROCESSORS_CONF, access
#include <netinet/in.h>     // IPPROTO_TCP
#include <netinet/tcp.h>    // TCP_NODELAY, TCP_QUICKACK
#include <sys/socket.h>     // setsockopt, SO_SNDBUF
#include <gst/rtsp/gstrtspconnection.h>  // gst_rtsp_connection_get_write_socket
#include "include/gst_element_print_properties.h"

#define LOG_TAG "strmout_Cam2Streamer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,    LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,   LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG,  LOG_TAG, __VA_ARGS__)
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Module-level state (one RTSP server instance at a time).
// Protected by s_lock; all public JNI functions acquire it before access.
// ---------------------------------------------------------------------------
static pthread_mutex_t s_lock     = PTHREAD_MUTEX_INITIALIZER;
static GstRTSPServer  *s_server   = NULL;    // RTSP server; NULL until init
static GstElement     *s_appsrc   = NULL;    // set in on_media_configure when first client connects
static GstElement     *s_audio_appsrc = NULL; // audio appsrc; set alongside s_appsrc
static bool            s_audio_need_data = true; // audio flow-control flag
static GMainLoop      *s_loop     = NULL;
static GMainContext   *s_context  = NULL;
static GSource        *s_periodic_source = NULL; // 30 s element-dump timer
static GstBin         *s_full_pipeline   = NULL; // full pipeline cached from on_client_play_request
static pthread_t       s_loop_tid;
static bool            s_loop_tid_valid = false;
// Set to true each time on_media_configure fires (a new/re-connecting RTSP
// client starts playing).  Consumed by nativeConsumeKeyframeRequest() so that
// Java can immediately request a sync frame from the encoder, preventing
// h265parse from dropping all P/B frames that precede the next IDR.
static bool            s_request_keyframe = false;
// Appsrc flow-control flag: set true by the "need-data" signal, cleared by
// "enough-data".  nativePushEncodedBuffer() drops P/B frames while false.
// Initialised to true so the first IDR frames are pushed as soon as the
// appsrc is ready, without waiting for the initial need-data signal.
// Written lock-free (plain bool) – see deadlock note above.
static bool            s_need_data = true;

// True only while the current RTSP media is fully prepared (pipeline in
// PAUSED/PLAYING).  Set true in on_media_prepared, cleared in
// on_media_configure (new media being built) and on_media_unprepared
// (media torn down).  nativePushEncodedBuffer() gates ALL pushes on this so
// it never feeds the appsrc during the configure→prepared window, when the
// appsrc pointer is already valid but its srcpad is still flushing
// (which would otherwise return GST_FLOW_FLUSHING / -2).
static bool            s_media_prepared = false;

// Number of RTSP clients currently connected.  Incremented in
// on_client_connected, decremented in on_client_closed.  Protected by s_lock.
static int             s_client_count = 0;

// Base camera PTS (ns) for the current session.  Set to the first buffer's
// presentationTimeUs on the first push; all subsequent GST_BUFFER_PTS values
// are relative to this base so GStreamer sees a clean t=0 timeline.
// Camera hardware timestamps have sub-100 µs jitter, giving the receiver
// near-perfect RTP clock spacing and allowing a minimal jitter-buffer depth.
static GstClockTime    s_pts_base  = GST_CLOCK_TIME_NONE;

// ---------------------------------------------------------------------------
// A/V sync diagnostics – updated under s_lock on every push.
//
// WHY audio latency matters: in VIDEO_AND_AUDIO_BOTH mode the RTSP client
// runs an A/V synchroniser that computes the offset between the RTP
// timestamps of the two streams (via RTCP SR NTP↔RTP mappings).  If audio
// PTS is N ms ahead of the matching video PTS in the GStreamer timeline, the
// client holds video back by N ms waiting for audio to "catch up", adding N ms
// of extra glass-to-glass latency beyond the VIDEO_ONLY baseline.
//
// s_last_video_pts_ns feeds the EMA that corrects audio PTS.
// s_av_offset_ema is the smoothed estimate of (raw_audio_pts − last_video_pts).
// MEASURED (logcat.txt, TSx, 2026-06-17): this offset is POSITIVE, ≈ +66..+110 ms
// (mean ≈ +85 ms) — audio PTS is AHEAD of video, every frame.  Audio and video
// share one clock (CLOCK_BOOTTIME, confirmed via SENSOR_INFO_TIMESTAMP_SOURCE=1),
// so both PTSs are honest capture-times; the freshest pushed audio frame carries
// a NEWER capture-start timestamp than the freshest pushed video frame (the
// back-dated audio capture time under-counts HAL buffering relative to the
// sensor exposure timestamp).  The EMA tracks this ~+85 ms offset and the
// correction below subtracts it: corrected_pts = raw_audio_pts − s_av_offset_ema,
// shifting audio back onto the video timeline.  The EMA is ACTIVE and effective —
// the post-correction residual oscillates ≈ ±20 ms around 0.  (An earlier run
// reported a NEGATIVE / audio-behind offset; it does not reproduce on this build.)
//
// Convergence: with α=0.15, the EMA reaches 99% of the true value after
// log(0.01)/log(0.85) ≈ 29 samples ≈ 29 × 23 ms ≈ 670 ms.  On the very first
// sample the EMA is initialised to the measured offset (no cold-start penalty).
// ---------------------------------------------------------------------------
static GstClockTime    s_last_video_pts_ns = GST_CLOCK_TIME_NONE;
static GstClockTime    s_av_offset_ema     = GST_CLOCK_TIME_NONE; // EMA of (audio_pts − video_pts)
static const double    AV_OFFSET_ALPHA     = 0.15;                // EMA smoothing factor
static int             s_video_push_count  = 0;   // periodic video log counter
static int             s_audio_push_count  = 0;   // periodic audio log counter

// CPU core selected for the data-path threads (GMainLoop + encoder callback).
// Set once by loop_thread_fn to the highest-numbered online CPU (typically the
// prime/big core on Qualcomm Snapdragon).  Read by nativePushEncodedBuffer to
// pin the encoder callback thread to the same core on its first invocation.
static volatile int    s_perf_cpu        = -1;
// True once the encoder callback thread has been pinned.
// Read+written only under s_lock.
static bool            s_encoder_pinned  = false;

// ---------------------------------------------------------------------------
// Runtime feature flags – each can be changed at any time via the
// corresponding nativeSet*() JNI method before or after pipeline init.
// All flags are protected by s_lock where noted.
// ---------------------------------------------------------------------------
// Pin the GMainLoop thread and the encoder callback thread to the same
// high-performance CPU core.  Default: disabled (false).
static bool s_cpu_affinity_enable    = false;
// Set TCP_NODELAY (and TCP_QUICKACK) on the RTSP/RTP control socket to
// disable Nagle's algorithm and flush every write immediately.
// Default: disabled (false).
static bool s_tcp_no_delay           = false;
// Shrink the kernel send buffer to limit how many unsent bytes the kernel
// queues for this socket.  Value in bytes; 0 = leave at kernel default.
// Recommended: 65536 (64 KB ≈ 4 frames at 20 Mbps/30 fps) so stale frames
// cannot pile up behind a fresh one.  Default: 0 (disabled).
static int  s_so_sndbuf_size         = 0;
// Set TCP_NOTSENT_LOWAT to cap the amount of data the kernel holds in the
// send buffer that has not yet been handed to the NIC.  When the unsent
// backlog exceeds this threshold the socket becomes non-writable (EPOLLOUT
// clears), letting GStreamer apply back-pressure and drop stale frames.
// Value in bytes; 0 = disabled (leave at kernel default, usually unlimited).
// Recommended: 16384 (16 KB) on kernels that support it (Linux ≥ 4.4).
// Default: 0 (disabled).
static int  s_tcp_notsent_lowat      = 0;
// Set IP_TOS to DSCP EF (Expedited Forwarding, 0xb8) for priority queuing
// through managed switches/routers.  Default: disabled (false).
static bool s_ip_tos_dscp_ef         = false;
// Include h264parse/h265parse in the GStreamer pipeline.  When false the
// payloader receives raw Annex-B AUs directly from appsrc, saving one
// frame of parse latency.  Default: disabled (false = no parser).
// Consumed in Java's initGstPipeline(); read from C++ is not needed.
// (Declaration kept here for symmetry; setter below.)
static bool s_h26x_parse_enable      = false;
// Set rtpbin buffer-mode=none (0) to disable the sender-side jitter buffer.
// do-lost=FALSE and latency=0 are always applied regardless of this flag.
// Default: true (buffer-mode=none applied – lowest sender latency).
static bool s_rtpbin_buffer_mode_none = true;
// Fire on_periodic_element_dump every 30 s to log all element properties.
// Default: disabled (false) to avoid logcat noise in production.
static bool s_periodic_dump_enable   = false;
// Interval in seconds between periodic pipeline dumps (element properties
// + optional DOT file).  Default: 30 s.  Changed via nativeSetPipelineDumpInterval().
static guint s_pipeline_dump_interval_seconds = 30;
// Enable pipeline DOT file dumping alongside periodic element dumps.
// When true, dump_pipeline_to_dot_file() is called from the periodic timer
// callback (limited to 3 files per session to avoid filling storage).
// Default: disabled (false).
static bool s_pipeline_dot_dump_enable = false;
// Output directory for DOT files.  Set via nativeSetPipelineDotOutputDir().
// Empty string means use the default (/data/user/0/com.crestron.streamout/files/).
static char s_pipeline_dot_output_dir[512] = "/data/user/0/com.crestron.streamout/files/";
// Number of DOT files written during this session (capped at 3).
static unsigned int s_pipeline_dot_periodic_dump_count = 0;

// Advertise RTCP feedback (a=rtcp-fb nack/pli/fir) in the RTSP SDP and restrict
// the media factory to the AVPF profile so a SINGLE m=video section is emitted.
// When false the server advertises plain RTP/AVP only (also a single m=video)
// and the CresRTSPMedia subclass / pay0 caps probe are not installed.
// Default: disabled (false).
static bool s_enable_rtcp_feedback = false;

// Codec type detected from the pipeline string; used to set appsrc caps
// (required for the pipeline to negotiate caps and reach PAUSED/preroll).
typedef enum { CODEC_H264 = 0, CODEC_H265 = 1 } CodecType;
static CodecType s_codec_type = CODEC_H264;

// Stream mode: controls which appsrc elements are created in the GStreamer
// pipeline and which capture paths (video encoder / audio record) are active.
typedef enum {
    STREAM_VIDEO_ONLY      = 0,   // video appsrc only – no audio branch
    STREAM_AUDIO_ONLY      = 1,   // audio appsrc only – no video branch
    STREAM_VIDEO_AND_AUDIO = 2    // both branches (default)
} StreamMode;
static StreamMode s_stream_mode = STREAM_VIDEO_AND_AUDIO;

// Audio AAC capture format, detected from the pipeline string in
// nativeGstPipelineInit; used by on_media_configure to build the audio appsrc
// caps + codec_data (ASC) so they match whatever Java captured/encoded.
// (Java is the source of truth – it bakes rate/channels into the launch-string
// caps; these just let the native preroll caps-set stay consistent.)
static int s_audio_sample_rate = 44100;   // Hz   (44100, 48000, ...)
static int s_audio_channels    = 1;       // 1=mono, 2=stereo
// True when the audio appsrc carries raw PCM for Opus (opusenc in the pipeline)
// rather than AAC access units.  Detected from the pipeline string in
// nativeGstPipelineInit (presence of "opusenc").  Selects PCM vs AAC caps in
// on_media_configure and PCM vs AAC frame duration in nativePushAudioBuffer.
static bool s_audio_raw_pcm    = false;

// AAC AudioSpecificConfig sampling-frequency index for `rate`
// (ISO/IEC 14496-3 Table 1.16), or -1 if unsupported.  Used to build codec_data
// in on_media_configure so it matches the runtime rate.
static int aac_freq_index(int rate) {
    switch (rate) {
        case 96000: return 0;  case 88200: return 1;  case 64000: return 2;
        case 48000: return 3;  case 44100: return 4;  case 32000: return 5;
        case 24000: return 6;  case 22050: return 7;  case 16000: return 8;
        case 12000: return 9;  case 11025: return 10; case 8000:  return 11;
        default:    return -1;
    }
}

// Transport mode: selects the RTP lower-transport protocol advertised by
// the RTSP server.  Applies to all streams in the session (video + audio).
// TRANSPORT_TCP (default): RTP is interleaved over the RTSP control TCP
//   connection (RFC 2326 §10.12).  Works through NAT/firewalls.
// TRANSPORT_UDP: RTP travels over separate UDP unicast sockets.  Lower
//   per-packet overhead but requires reachable UDP ports.
typedef enum {
    TRANSPORT_TCP = 0,   // GST_RTSP_LOWER_TRANS_TCP (default)
    TRANSPORT_UDP = 1    // GST_RTSP_LOWER_TRANS_UDP
} TransportMode;
#ifdef USE_RTSP_LOWER_TRANS_TCP_ONLY
static TransportMode s_transport_mode = TRANSPORT_TCP;
#endif

extern void streamout_status_fb(int stream_id, int status_code, const char* info);

//must mathc with the definition in grpc proto file
enum Cam2Streamer_status_enum {
  stream_status_unspecified = 0,
  stream_status_ready = 1,
  stream_status_client_connected = 2,
  stream_status_stopped = 3,
};

// ---------------------------------------------------------------------------
// dump_pipeline_to_dot_file – Dump GStreamer pipeline to DOT file for
// visualization.
//
// Creates a file in the configured directory (gst-pipeline-*.dot) that can
// be converted to PNG:
//   dot -Tpng <file>.dot -o <file>.png
//
// Uses gst_debug_bin_to_dot_data() and writes the string manually for
// reliable file creation (more reliable than the
// GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS macro which depends on env vars).
//
// NOTE: runs on the GMainLoop thread; no s_lock needed for read-only
// pipeline access.
// ---------------------------------------------------------------------------
static void dump_pipeline_to_dot_file(GstBin *pipeline, const char *suffix)
{
    if (!pipeline) {
        LOGE("dump_pipeline_to_dot_file: invalid pipeline");
        return;
    }

    // Use configured output directory (or default if not set)
    const char *output_dir = (s_pipeline_dot_output_dir[0] != '\0')
        ? s_pipeline_dot_output_dir
        : "/data/user/0/com.crestron.streamout/files/";

    // Ensure directory path ends with /
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s", output_dir);
    size_t dir_len = strlen(dir_path);
    if (dir_len > 0 && dir_len < sizeof(dir_path) - 1 && dir_path[dir_len - 1] != '/') {
        dir_path[dir_len] = '/';
        dir_path[dir_len + 1] = '\0';
    }

    // Validate directory exists and is writable
    if (access(dir_path, F_OK) != 0) {
        LOGE("dump_pipeline_to_dot_file: output directory does not exist: %s", dir_path);
        LOGE("dump_pipeline_to_dot_file: please create the directory manually");
        return;
    }
    if (access(dir_path, W_OK) != 0) {
        LOGE("dump_pipeline_to_dot_file: no write permission for directory: %s", dir_path);
        LOGE("dump_pipeline_to_dot_file: error: %s", strerror(errno));
        return;
    }
    LOGD("dump_pipeline_to_dot_file: output directory validated: %s", dir_path);

    // Generate DOT data
    LOGD("dump_pipeline_to_dot_file: generating DOT file data");
    gchar *dot_data = gst_debug_bin_to_dot_data(pipeline, GST_DEBUG_GRAPH_SHOW_ALL);
    if (!dot_data) {
        LOGE("dump_pipeline_to_dot_file: gst_debug_bin_to_dot_data returned NULL");
        return;
    }

    // Create filename with timestamp
    char timestamp_str[64];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(timestamp_str, sizeof(timestamp_str), "%Y%m%d-%H%M%S", tm_info);

    char dot_filename[768];
    snprintf(dot_filename, sizeof(dot_filename), "%sgst-pipeline-%s.%s.dot",
             dir_path, suffix, timestamp_str);

    LOGD("dump_pipeline_to_dot_file: writing DOT file to %s", dot_filename);

    // Write DOT data to file
    FILE *dot_file = fopen(dot_filename, "w");
    if (!dot_file) {
        LOGE("dump_pipeline_to_dot_file: failed to open file %s: %s",
             dot_filename, strerror(errno));
        g_free(dot_data);
        return;
    }

    size_t data_len = strlen(dot_data);
    size_t written = fwrite(dot_data, 1, data_len, dot_file);
    if (written != data_len) {
        LOGE("dump_pipeline_to_dot_file: write error: wrote %zu of %zu bytes to %s",
             written, data_len, dot_filename);
        fclose(dot_file);
        g_free(dot_data);
        return;
    }

    fclose(dot_file);
    g_free(dot_data);

    LOGI("dump_pipeline_to_dot_file: pipeline DOT file created: %s", dot_filename);
    LOGI("dump_pipeline_to_dot_file:   to convert to PNG: dot -Tpng %s -o %s.png",
         dot_filename, dot_filename);
}

// ---------------------------------------------------------------------------
// dump_audio_video_pipeline – Periodic GSource callback that performs a
// comprehensive pipeline dump: element hierarchy with state, pad caps,
// all properties, and optional DOT file generation.
//
// Mirrors dump_audio_video_pipeline() from CresGStreamerJni.cpp, adapted
// for the module-level state used in camera2_gst_streamer.
//
// Runs on the GMainLoop thread.  Returns G_SOURCE_CONTINUE to keep the
// timer alive.
// ---------------------------------------------------------------------------
static gboolean dump_audio_video_pipeline(gpointer /*user_data*/)
{
    // Snapshot + ref the cached pipeline under s_lock so a concurrent
    // nativeGstPipelineDestroy (JNI thread) cannot NULL/free it mid-dump.
    // Reading the global lock-free and dereferencing it across the many
    // GStreamer calls below would be a NULL-deref / use-after-free.
    pthread_mutex_lock(&s_lock);
    GstBin *pipeline = s_full_pipeline
        ? GST_BIN(gst_object_ref(s_full_pipeline)) : NULL;
    pthread_mutex_unlock(&s_lock);

    if (!pipeline) {
        LOGI("dump_audio_video_pipeline: pipeline not ready yet – no client has played");
        return G_SOURCE_CONTINUE;
    }

    LOGI(" ");
    LOGI("===== AUDIO/VIDEO PIPELINE DUMP (TIMER FIRED) =====");

    // Log pipeline state
    GstState current_state;
    gst_element_get_state(GST_ELEMENT(pipeline), &current_state, NULL, 0);
    LOGI("  Pipeline State: %s", gst_element_state_get_name(current_state));

    // Log cached appsrc elements
    LOGI("=== Cached Key Elements ===");
    {
        // Read appsrc pointers; these are only modified under s_lock when the
        // pipeline is being set up or torn down, and this callback runs on the
        // GMainLoop thread after setup is complete, so a brief lock is safe.
        pthread_mutex_lock(&s_lock);
        GstElement *v_appsrc = s_appsrc ? (GstElement *)gst_object_ref(s_appsrc) : NULL;
        GstElement *a_appsrc = s_audio_appsrc ? (GstElement *)gst_object_ref(s_audio_appsrc) : NULL;
        pthread_mutex_unlock(&s_lock);

        if (v_appsrc) {
            LOGI("  Video Appsrc: %s", GST_ELEMENT_NAME(v_appsrc));
            gst_element_print_properties(v_appsrc);
            gst_object_unref(v_appsrc);
        } else {
            LOGI("  Video Appsrc: (none)");
        }
        if (a_appsrc) {
            LOGI("  Audio Appsrc: %s", GST_ELEMENT_NAME(a_appsrc));
            gst_element_print_properties(a_appsrc);
            gst_object_unref(a_appsrc);
        } else {
            LOGI("  Audio Appsrc: (none)");
        }
    }

    // Full pipeline hierarchy traversal with element classification
    LOGI("=== Full Pipeline Hierarchy ===");
    {
        GstIterator *it = gst_bin_iterate_recurse(pipeline);
        if (it) {
            GValue item = G_VALUE_INIT;
            GstIteratorResult res;
            gboolean done = FALSE;
            int total = 0;
            while (!done) {
                res = gst_iterator_next(it, &item);
                switch (res) {
                    case GST_ITERATOR_OK: {
                        GstElement *elem = GST_ELEMENT(g_value_get_object(&item));
                        if (elem) {
                            total++;
                            const gchar *elem_name = GST_ELEMENT_NAME(elem);
                            const gchar *elem_type = G_OBJECT_TYPE_NAME(elem);

                            GstState elem_state;
                            gst_element_get_state(elem, &elem_state, NULL, 0);

                            // Classify element
                            const char *tag = "";
                            if (strstr(elem_type, "Audio") || strstr(elem_type, "audio") ||
                                strstr(elem_type, "aac")   || strstr(elem_type, "Aac") ||
                                strstr(elem_name, "audio") || strstr(elem_name, "aac")) {
                                tag = "[AUDIO] ";
                            } else if (strstr(elem_type, "Video")  || strstr(elem_type, "video") ||
                                       strstr(elem_type, "h264")   || strstr(elem_type, "H264") ||
                                       strstr(elem_type, "h265")   || strstr(elem_type, "H265") ||
                                       strstr(elem_type, "Rtp")    || strstr(elem_type, "rtp")  ||
                                       strstr(elem_name, "src")    || strstr(elem_name, "pay")) {
                                tag = "[VIDEO] ";
                            }

                            LOGI("  [%d] %s%s (type=%s, state=%s)",
                                 total, tag, elem_name, elem_type,
                                 gst_element_state_get_name(elem_state));

                            // Log pads and current caps
                            GstIterator *pad_it = gst_element_iterate_pads(elem);
                            if (pad_it) {
                                GValue pad_item = G_VALUE_INIT;
                                while (gst_iterator_next(pad_it, &pad_item) == GST_ITERATOR_OK) {
                                    GstPad *pad = GST_PAD(g_value_get_object(&pad_item));
                                    if (pad) {
                                        const gchar *pad_name = GST_PAD_NAME(pad);
                                        GstCaps *caps = gst_pad_get_current_caps(pad);
                                        LOGI("    Pad: %s%s", pad_name,
                                             GST_PAD_IS_LINKED(pad) ? " (linked)" : " (unlinked)");
                                        if (caps) {
                                            gchar *caps_str = gst_caps_to_string(caps);
                                            LOGI("      Caps: %s", caps_str);
                                            g_free(caps_str);
                                            gst_caps_unref(caps);
                                        }
                                    }
                                    g_value_reset(&pad_item);
                                }
                                g_value_unset(&pad_item);
                                gst_iterator_free(pad_it);
                            }

                            // Log all properties
                            gst_element_print_properties(elem);
                        }
                        g_value_reset(&item);
                        break;
                    }
                    case GST_ITERATOR_RESYNC:
                        gst_iterator_resync(it);
                        break;
                    case GST_ITERATOR_DONE:
                    case GST_ITERATOR_ERROR:
                    default:
                        done = TRUE;
                        break;
                }
            }
            g_value_unset(&item);
            gst_iterator_free(it);
            LOGI("=== dump complete – %d element(s) ===", total);
        }
    }

    // DOT file generation (limited to 3 per session)
    if (s_pipeline_dot_dump_enable) {
        s_pipeline_dot_periodic_dump_count++;
        if (s_pipeline_dot_periodic_dump_count <= 3) {
            char dot_suffix[64];
            snprintf(dot_suffix, sizeof(dot_suffix), "periodic-%u",
                     s_pipeline_dot_periodic_dump_count);
            dump_pipeline_to_dot_file(pipeline, dot_suffix);
        } else if (s_pipeline_dot_periodic_dump_count == 4) {
            LOGI("dump_audio_video_pipeline: pipeline DOT file creation limit reached (3 files max)");
        }
    }

    LOGI("===== END PIPELINE DUMP =====");
    LOGI(" ");

    gst_object_unref(pipeline);
    return G_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------------------
// on_periodic_element_dump – GSource callback fired every 30 seconds.
//
// Walks the full GStreamer pipeline (parent of the RTSP media user-bin) and
// calls gst_element_print_properties() on every element, mirroring the
// PRINT_ELEMENT_PROPERTY allvideo / allav debug command in jni.cpp.
// The dump is useful for diagnosing runtime property changes (ts-offset,
// sync, buffer levels, etc.) without needing a manual debug command.
//
// NOTE: runs on the GMainLoop thread (no s_lock needed for read-only
// property access; gst_element_print_properties is read-only).
// ---------------------------------------------------------------------------
static gboolean on_periodic_element_dump(gpointer /*user_data*/)
{
    // s_full_pipeline is cached (with a ref) in on_client_play_request the first
    // time a client plays, and released in on_media_unprepared / by
    // nativeGstPipelineDestroy.  nativeGstPipelineDestroy runs on a JNI thread
    // and can NULL/unref s_full_pipeline concurrently with this GMainLoop
    // callback, so take a snapshot ref under s_lock and operate on the local
    // copy – dereferencing the global lock-free here would risk a NULL-deref/UAF.
    pthread_mutex_lock(&s_lock);
    GstBin *pipeline = s_full_pipeline
        ? GST_BIN(gst_object_ref(s_full_pipeline)) : NULL;
    pthread_mutex_unlock(&s_lock);

    if (!pipeline) {
        LOGI("on_periodic_element_dump: pipeline not ready yet – no client has played");
        return G_SOURCE_CONTINUE;
    }

    LOGI("on_periodic_element_dump: === 30-second pipeline element dump ===");
    LOGI("on_periodic_element_dump: pipeline '%s'",
         GST_ELEMENT_NAME(pipeline));

    GstIterator *it = gst_bin_iterate_recurse(pipeline);
    if (it) {
        GValue item = G_VALUE_INIT;
        GstIteratorResult res;
        gboolean done = FALSE;
        int total = 0;
        while (!done) {
            res = gst_iterator_next(it, &item);
            switch (res) {
                case GST_ITERATOR_OK: {
                    GstElement *elem = GST_ELEMENT(g_value_get_object(&item));
                    if (elem) {
                        total++;
                        LOGI("on_periodic_element_dump: [%d] %s (%s)",
                             total,
                             GST_ELEMENT_NAME(elem),
                             G_OBJECT_TYPE_NAME(elem));
                        gst_element_print_properties(elem);
                    }
                    g_value_reset(&item);
                    break;
                }
                case GST_ITERATOR_RESYNC:
                    gst_iterator_resync(it);
                    break;
                case GST_ITERATOR_DONE:
                case GST_ITERATOR_ERROR:
                default:
                    done = TRUE;
                    break;
            }
        }
        g_value_unset(&item);
        gst_iterator_free(it);
        LOGI("on_periodic_element_dump: === dump complete - %d element(s) ===", total);
    }

    gst_object_unref(pipeline);
    return G_SOURCE_CONTINUE;  // keep firing every 30 s
}

// ---------------------------------------------------------------------------
// pick_perf_cpu – return the highest-numbered online CPU, which on Qualcomm
// Snapdragon is typically the prime/big core (e.g. core 7 on an octa-core
// 8-series SoC).
//
// /sys/devices/system/cpu/cpu0/online does not exist (cpu0 is always on);
// we skip it and return it as the fallback if nothing else is online.
// ---------------------------------------------------------------------------
static int pick_perf_cpu(void)
{
    int nconf = (int)sysconf(_SC_NPROCESSORS_CONF);
    for (int i = nconf - 1; i > 0; i--) {
        char path[64];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/online", i);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int online = 0;
        fscanf(f, "%d", &online);
        fclose(f);
        if (online) {
            LOGI("pick_perf_cpu: selected CPU %d (of %d configured)", i, nconf);
            return i;
        }
    }
    LOGI("pick_perf_cpu: fallback to CPU 0 (of %d configured)", nconf);
    return 0;
}

// ---------------------------------------------------------------------------
// pin_current_thread_to_cpu – pin the calling thread to a single CPU core.
//
// Keeping the encoder callback thread and the GMainLoop thread on the same
// physical core ensures their working sets (appsrc queue head, RTP state)
// stay hot in L1/L2 cache and eliminates inter-core cache-coherency traffic
// on the data path.  setsockopt writes (TCP_NODELAY) and the appsink push
// all happen on the encoder callback thread in a pipelined fashion.
// ---------------------------------------------------------------------------
static void pin_current_thread_to_cpu(int cpu)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    // pid=0 means the calling thread.
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0) {
        LOGI("pin_current_thread_to_cpu: thread pinned to CPU %d", cpu);
    } else {
        LOGW("pin_current_thread_to_cpu: sched_setaffinity(CPU %d) failed errno=%d (%s)",
             cpu, errno, strerror(errno));
    }
}

// ---------------------------------------------------------------------------
// GMainLoop thread – runs the GLib main loop so the RTSP server can accept
// client connections and dispatch GStreamer bus messages.
// ---------------------------------------------------------------------------
static void *loop_thread_fn(void *arg)
{
    GMainLoop *loop = static_cast<GMainLoop *>(arg);
    GMainContext *ctx = g_main_loop_get_context(loop);

    // Optionally pin this thread to the highest-performance CPU core.
    // The encoder callback thread is pinned to the same core on its first
    // call to nativePushEncodedBuffer() when cpu_affinity_enable is true.
    if (s_cpu_affinity_enable) {
        int perf_cpu = pick_perf_cpu();
        s_perf_cpu = perf_cpu;
        pin_current_thread_to_cpu(perf_cpu);
        LOGI("GMain loop starting on CPU %d (affinity enabled)", perf_cpu);
    } else {
        LOGI("GMain loop starting (CPU affinity disabled)");
    }

    // Make this context the thread-default so GStreamer's internal async
    // scheduling (g_main_context_get_thread_default) uses the right context.
    g_main_context_push_thread_default(ctx);
    g_main_loop_run(loop);
    LOGI("GMain loop exited");
    g_main_context_pop_thread_default(ctx);
    return NULL;
}

// ---------------------------------------------------------------------------
// Appsrc flow-control callbacks – mirror cb_srcNeedData / cb_srcEnoughData
// in cresStreamOutManager.cpp.
//
// NOTE: these callbacks intentionally do NOT acquire s_lock.
// nativePushEncodedBuffer() holds s_lock while calling
// gst_app_src_push_buffer(), which can emit "enough-data" synchronously in
// the same thread.  If the callback also tried to lock s_lock, the thread
// would deadlock with itself.  A bare bool store is atomic on all supported
// ABIs (ARMv7, ARM64, x86) so the lock-free write is safe.
// ---------------------------------------------------------------------------
static void cb_src_need_data(GstElement * /*appsrc*/, guint /*size*/, gpointer /*user_data*/)
{
    s_need_data = true;
}

static void cb_src_enough_data(GstElement * /*appsrc*/, gpointer /*user_data*/)
{
    s_need_data = false;
}

static void cb_audio_need_data(GstElement * /*appsrc*/, guint /*size*/, gpointer /*user_data*/)
{
    s_audio_need_data = true;
}

static void cb_audio_enough_data(GstElement * /*appsrc*/, gpointer /*user_data*/)
{
    s_audio_need_data = false;
}

// ---------------------------------------------------------------------------
// Opus encode-latency probe
//
// Measures the wall-clock time a buffer spends inside opusenc (sink-pad arrival
// -> src-pad output), i.e. the actual Opus compute that the Java-side
// "Opus push latency" number does NOT include.
//
// opusenc is a chain-based GstAudioEncoder: for a given input buffer the encode
// runs synchronously and the encoded buffer is pushed downstream on the SAME
// streaming thread, before the chain call returns. Sink and src probes
// therefore execute on one streaming thread (the queue feeding opusenc), which
// is NOT the Java audio thread and does NOT hold s_lock. We still guard the
// shared stats with a dedicated mutex so the design is robust to any future
// threading changes; we deliberately avoid s_lock to prevent contention with
// the appsrc push path.
//
// Input PCM is delivered as fixed 10 ms frames (frame-size=10), so opusenc
// consumes one input buffer and emits one output buffer, in order, on the same
// streaming thread. We therefore pair sink->src buffers in FIFO order (oldest
// outstanding sink arrival pops on each src buffer) rather than by PTS: opusenc
// is a GstAudioEncoder and re-timestamps its output for the encoder delay, so
// the src-pad PTS does NOT equal the sink-pad PTS and a PTS-keyed lookup would
// never match. The map is keyed by sink PTS only to preserve insertion order
// (PTS is monotonically increasing, so map.begin() is the oldest buffer).
// ---------------------------------------------------------------------------
static pthread_mutex_t s_opus_probe_lock = PTHREAD_MUTEX_INITIALIZER;
static std::map<GstClockTime, gint64> s_opus_enc_in;   // sink PTS(ns) -> sink arrival (monotonic us), FIFO-ordered
static gint64 s_opus_enc_lat_min_us = G_MAXINT64;
static gint64 s_opus_enc_lat_max_us = 0;
static gint64 s_opus_enc_lat_sum_us = 0;
static int    s_opus_enc_lat_count  = 0;

// Master on/off for ALL native audio-latency diagnostic logging: the "Opus
// encode latency" pad-probe print and the "[A/V sync]" / "[EMA init]" PTS
// prints.  Default OFF (no logging; the probe callbacks also early-return so
// they cost nothing).  Toggled at runtime via the cam2_streamer_debug
// SET_AUDIO_LATENCY_DEBUG command (Cam2Streamer_SetAudioLatencyDebug below).
// LOGGING ONLY — when enabled, the probe stats and the EMA PTS correction run;
// this just gates the LOGI calls.
static volatile int s_audio_latency_debug = 0;

static void opus_probe_reset(void)
{
    pthread_mutex_lock(&s_opus_probe_lock);
    s_opus_enc_in.clear();
    s_opus_enc_lat_min_us = G_MAXINT64;
    s_opus_enc_lat_max_us = 0;
    s_opus_enc_lat_sum_us = 0;
    s_opus_enc_lat_count  = 0;
    pthread_mutex_unlock(&s_opus_probe_lock);
}

static GstPadProbeReturn opus_enc_sink_probe(GstPad * /*pad*/, GstPadProbeInfo *info, gpointer /*user_data*/)
{
    // Logging disabled: skip all bookkeeping (no lock, no map alloc) so the
    // probe is effectively free.  The src probe also early-returns, so the two
    // stay balanced; on re-enable both start fresh and the FIFO re-pairs.
    if (!s_audio_latency_debug)
        return GST_PAD_PROBE_OK;

    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buf) {
        GstClockTime pts = GST_BUFFER_PTS(buf);
        if (GST_CLOCK_TIME_IS_VALID(pts)) {
            gint64 now = g_get_monotonic_time();   // microseconds
            pthread_mutex_lock(&s_opus_probe_lock);
            s_opus_enc_in[pts] = now;
            // Cap the map so a re-chunking/stalled encoder can never leak it.
            while (s_opus_enc_in.size() > 64)
                s_opus_enc_in.erase(s_opus_enc_in.begin());
            pthread_mutex_unlock(&s_opus_probe_lock);
        }
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn opus_enc_src_probe(GstPad * /*pad*/, GstPadProbeInfo *info, gpointer /*user_data*/)
{
    // Logging disabled: skip all bookkeeping (no lock, no map work) so the probe
    // is effectively free.  Mirrors the early-return in opus_enc_sink_probe.
    if (!s_audio_latency_debug)
        return GST_PAD_PROBE_OK;

    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf)
        return GST_PAD_PROBE_OK;

    gint64 now   = g_get_monotonic_time();
    gint64 in_us = -1;
    gint64 mn = 0, mx = 0, sum = 0;
    int    n  = 0;

    pthread_mutex_lock(&s_opus_probe_lock);
    // FIFO pairing: the oldest outstanding sink buffer corresponds to this
    // encoded output buffer (opusenc is in-order 1:1). map.begin() is the
    // smallest sink PTS = the oldest arrival.
    if (!s_opus_enc_in.empty()) {
        std::map<GstClockTime, gint64>::iterator it = s_opus_enc_in.begin();
        in_us = it->second;
        s_opus_enc_in.erase(it);
    }
    if (in_us >= 0) {
        gint64 lat = now - in_us;               // microseconds inside opusenc
        if (lat < 0) lat = 0;                   // guard against clock skew
        if (lat < s_opus_enc_lat_min_us) s_opus_enc_lat_min_us = lat;
        if (lat > s_opus_enc_lat_max_us) s_opus_enc_lat_max_us = lat;
        s_opus_enc_lat_sum_us += lat;
        n   = ++s_opus_enc_lat_count;
        mn  = s_opus_enc_lat_min_us;
        mx  = s_opus_enc_lat_max_us;
        sum = s_opus_enc_lat_sum_us;
        if (n >= 100) {
            s_opus_enc_lat_min_us = G_MAXINT64;
            s_opus_enc_lat_max_us = 0;
            s_opus_enc_lat_sum_us = 0;
            s_opus_enc_lat_count  = 0;
        }
    }
    pthread_mutex_unlock(&s_opus_probe_lock);

    if (n >= 100 && s_audio_latency_debug) {
        LOGI("Opus encode latency: min=%.2f ms avg=%.2f ms max=%.2f ms "
             "(opusenc sink->src compute, last %d frames)",
             mn / 1000.0, (sum / (double)n) / 1000.0, mx / 1000.0, n);
    }
    return GST_PAD_PROBE_OK;
}

// ---------------------------------------------------------------------------
// on_bus_message – unified bus message handler (sync emission).
//
// Connected to the REAL media pipeline bus in on_media_prepared (NOT the
// configure-time bin bus, which never receives these messages – see the note
// in on_media_prepared).  Handles:
//   ERROR    – logs the GError + debug string at LOGE.
//   WARNING  – logs the GError + debug string at LOGW.
//   INFO     – logs the GError + debug string at LOGI.
//   LATENCY  – LOG ONLY.  We deliberately do NOT call
//              gst_bin_recalculate_latency() here.  Each transport sink
//              (appsink4/5/6/7…) posts its own GST_MESSAGE_LATENCY on its OWN
//              streaming thread, and sync emission runs this handler inline on
//              that thread.  Recalculating here therefore fired
//              gst_bin_recalculate_latency() concurrently from several
//              streaming (data-path) threads at once, and each recalc provoked
//              more LATENCY messages → a feedback storm that stalled RTP
//              delivery on reconnect ("did not really configure latency"
//              logged from multiple threads).  The authoritative, once-per-
//              media recalc is done in on_client_play_request on the GMainLoop
//              thread after the full sink set exists; that is sufficient.
//   STATE_CHANGED – LOG ONLY, and only for the top-level pipeline (every
//              element posts this, so it is filtered to the pipeline source to
//              avoid log spam).
//
// NOTE: fires on the GStreamer streaming thread (sync emission), so it must
// not block.  This handler now only logs – no pipeline mutation.
// user_data is the pipeline GstElement (ref held for the life of the closure);
// it is no longer dereferenced but the ref is still released by the
// GClosureNotify when the bus is finalized.
// ---------------------------------------------------------------------------
static void on_bus_message(GstBus * /*bus*/, GstMessage *msg, gpointer user_data)
{
    const gchar *src_name = GST_MESSAGE_SRC(msg)
        ? GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)) : "(unknown)";

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar  *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            LOGE("on_bus_message: ERROR from %s: %s",
                 src_name, err ? err->message : "(unknown)");
            if (dbg) { LOGE("on_bus_message: ERROR debug: %s", dbg); g_free(dbg); }
            if (err) g_error_free(err);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError *err = NULL;
            gchar  *dbg = NULL;
            gst_message_parse_warning(msg, &err, &dbg);
            LOGW("on_bus_message: WARNING from %s: %s",
                 src_name, err ? err->message : "(unknown)");
            if (dbg) { LOGW("on_bus_message: WARNING debug: %s", dbg); g_free(dbg); }
            if (err) g_error_free(err);
            break;
        }
        case GST_MESSAGE_INFO: {
            GError *err = NULL;
            gchar  *dbg = NULL;
            gst_message_parse_info(msg, &err, &dbg);
            LOGI("on_bus_message: INFO from %s: %s",
                 src_name, err ? err->message : "(unknown)");
            if (dbg) { LOGI("on_bus_message: INFO debug: %s", dbg); g_free(dbg); }
            if (err) g_error_free(err);
            break;
        }
        case GST_MESSAGE_LATENCY: {
            // LOG ONLY – do NOT recalculate here (see header comment).  The
            // authoritative recalc runs once in on_client_play_request on the
            // GMainLoop thread.  Recalculating from this streaming-thread
            // handler caused a concurrent recalc storm that stalled reconnects.
            (void)user_data;
            LOGI("on_bus_message: LATENCY from %s (log only; recalc handled in "
                 "on_client_play_request)", src_name);
            break;
        }
        case GST_MESSAGE_STATE_CHANGED: {
            // Every element posts STATE_CHANGED, which would flood the log, so
            // only report the top-level pipeline's transitions.  user_data is
            // the pipeline element (a ref held by the closure – see
            // on_media_prepared); compare the message source against it.
            if (GST_MESSAGE_SRC(msg) == (GstObject *) user_data) {
                GstState old_st, new_st, pending_st;
                gst_message_parse_state_changed(msg, &old_st, &new_st, &pending_st);
                LOGI("on_bus_message: STATE_CHANGED %s: %s -> %s (pending %s)",
                     src_name,
                     gst_element_state_get_name(old_st),
                     gst_element_state_get_name(new_st),
                     gst_element_state_get_name(pending_st));
            }
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// on_media_prepared – called by GstRTSPMedia after gst_rtsp_media_prepare()
// completes (the user bin + rtpbin/rtpsession are linked and in PAUSED state).
//
// This handler is now a lightweight diagnostic marker only: it records that
// the media reached PAUSED/PLAYING and returns.
//
// IMPORTANT: the transport sinks (appsink0/appsink1) are NOT present yet at
// "prepared" time.  gst_rtsp_stream_join_bin() adds them during SETUP, which
// runs AFTER this signal.  Empirically the recurse here only sees
// appsrc/queue/pay0/rtpbin/rtpsession – none of which carry the 20 ms
// processing-deadline – and gst_bin_recalculate_latency() called here runs
// against an incomplete sink set, so the bin logs "did not really configure
// latency".  Therefore BOTH the processing-deadline override and the latency
// recalculation are performed at the end of on_client_play_request instead,
// where the full sink set exists and the sinks are already set to
// sync=FALSE / processing-deadline=0.
//
// This handler IS, however, the correct place to attach bus message handlers:
// only now does the user bin have a parent GstPipeline whose bus actually
// receives error/warning/info/latency messages (see below).
// ---------------------------------------------------------------------------
static void on_media_prepared(GstRTSPMedia *media, gpointer /*user_data*/)
{
    LOGI("on_media_prepared: media prepared");

    // Diagnostic flag only.  nativePushEncodedBuffer() does NOT gate on this:
    // the authoritative gate is the s_appsrc NULL check (cleared in
    // on_media_unprepared).  s_media_prepared is kept purely for log tracing.
    pthread_mutex_lock(&s_lock);
    s_media_prepared = true;
    pthread_mutex_unlock(&s_lock);

    // -----------------------------------------------------------------------
    // Attach bus message handlers to the REAL media pipeline bus.
    //
    // on_media_configure runs BEFORE the media is prepared, so the only bus it
    // can obtain (gst_element_get_bus on the bare user bin) is the bin's OWN
    // private bus – NOT the bus elements post to once the bin is parented into
    // GstRTSPMedia's internal GstPipeline.  That is why the old configure-time
    // sync-message handler never fired.  Now that the media is prepared the
    // user bin has a parent pipeline; grab THAT pipeline's bus instead so
    // error / warning / info / latency messages are actually delivered.
    //
    // We use sync-message emission (not gst_bus_add_watch) because GstRTSPMedia
    // already owns the async watch on this bus, and because sync emission fires
    // on the posting thread regardless of the bus poll fd (the bin bus had
    // poll == NULL, which broke gst_bus_create_watch()).
    //
    // The factory is reusable=FALSE, so a fresh media + pipeline + bus is built
    // for every connect; these handlers die with the bus, so re-connecting here
    // each time is correct and leak-free.
    // -----------------------------------------------------------------------
    GstElement *user_bin     = gst_rtsp_media_get_element(media);
    GstObject  *pipeline_obj = user_bin
        ? gst_object_get_parent(GST_OBJECT(user_bin)) : NULL;
    if (user_bin) gst_object_unref(user_bin);

    if (pipeline_obj && GST_IS_ELEMENT(pipeline_obj)) {
        GstElement *pipeline = GST_ELEMENT(pipeline_obj);
        GstBus     *bus      = gst_element_get_bus(pipeline);
        if (bus) {
            gst_bus_enable_sync_message_emission(bus);
            // Connect one handler per message detail.  Each closure holds its
            // own ref to the pipeline (released by the GClosureNotify when the
            // bus is finalized) so the LATENCY case can safely recalculate.
            static const char *const k_details[] = {
                "sync-message::error",
                "sync-message::warning",
                "sync-message::info",
                "sync-message::latency",
                "sync-message::state-changed",
            };
            for (guint i = 0; i < G_N_ELEMENTS(k_details); i++) {
                g_signal_connect_data(bus, k_details[i],
                                      G_CALLBACK(on_bus_message),
                                      gst_object_ref(pipeline),
                                      (GClosureNotify) gst_object_unref,
                                      (GConnectFlags) 0);
            }
            gst_object_unref(bus);
            LOGI("on_media_prepared: bus handlers (error/warning/info/latency/"
                 "state-changed) connected on real pipeline '%s'",
                 GST_ELEMENT_NAME(pipeline));
        } else {
            LOGW("on_media_prepared: could not get pipeline bus – messages unhandled");
        }
        gst_object_unref(pipeline_obj);
    } else {
        LOGW("on_media_prepared: user bin has no parent pipeline – bus handlers not attached");
        if (pipeline_obj) gst_object_unref(pipeline_obj);
    }

    LOGI("on_media_prepared: media reached PAUSED/PLAYING");
}

// ---------------------------------------------------------------------------
// on_media_unprepared – called by GstRTSPMedia when the media is unprepared
// (the last client disconnected and the media bin is being driven to NULL,
// then destroyed because the factory is set non-reusable).
//
// At this point the appsrc inside the media is stopped and its srcpad is
// flushing, so any gst_app_src_push_buffer() would return GST_FLOW_FLUSHING
// (-2).  Rather than keep pushing into a dead element and spamming errors,
// clear the cached appsrc handles here.  nativePushEncodedBuffer() then hits
// its `if (!s_appsrc) return;` guard and silently drops frames until the next
// client triggers a fresh on_media_configure, which re-acquires the live
// appsrc.  This is the clean "stop pushing while unprepared" gate.
//
// Runs on the GMainLoop thread; nativePushEncodedBuffer() runs on the encoder
// callback thread.  Both take s_lock, so swapping the pointers here is safe.
// ---------------------------------------------------------------------------
static void on_media_unprepared(GstRTSPMedia * /*media*/, gpointer /*user_data*/)
{
    LOGI("on_media_unprepared: media unprepared - clearing appsrc handles to stop pushes");
    pthread_mutex_lock(&s_lock);
    if (s_appsrc) {
        gst_object_unref(s_appsrc);
        LOGI("on_media_unprepared: set appsrc %p to NULL", s_appsrc);
        s_appsrc = NULL;
    }
    if (s_audio_appsrc) {
        gst_object_unref(s_audio_appsrc);
        LOGI("on_media_unprepared: set audio appsrc %p to NULL", s_audio_appsrc);
        s_audio_appsrc = NULL;
    }
    // Release the full-pipeline ref taken in on_client_play_request.  The media
    // (and its internal GstPipeline) is being destroyed here (factory
    // reusable=FALSE), so dropping our ref now avoids keeping a dead pipeline
    // alive and makes the periodic dump correctly report "no pipeline" until the
    // next client plays.  This is the authoritative unref site (symmetric with
    // the appsrc release just above); nativeGstPipelineDestroy keeps its own
    // guarded unref only as a shutdown safety net.  All three sites null the
    // pointer under s_lock, so none can double-unref.  GstRTSPMedia still owns
    // its own ref during teardown, so this unref will not finalize under the lock.
    if (s_full_pipeline) {
        gst_object_unref(s_full_pipeline);
        LOGI("on_media_unprepared: released cached full pipeline %p", s_full_pipeline);
        s_full_pipeline = NULL;
    }
    // Block flow-control until the next media is configured and prepared.
    s_need_data       = false;
    s_audio_need_data = false;
    s_media_prepared  = false;   // diagnostic flag reset (not a push gate)

    // Reset the PTS timeline so the NEXT media starts at a clean t=0.
    //
    // WHY THIS MATTERS
    // ----------------
    // s_pts_base is the first buffer's camera timestamp (ns); every buffer we
    // push uses GST_BUFFER_PTS = (camera_pts - s_pts_base), so the very first
    // frame of a session lands at running-time ≈ 0.  The camera clock is
    // monotonic and keeps advancing the whole time the device is up – it is
    // NOT reset between RTSP connections.
    //
    // If we do NOT clear s_pts_base on disconnect, the SECOND connection reuses
    // the FIRST connection's base.  Example seen in the logs: the device had
    // been streaming for ~12 s, so the reconnect's first frame produced
    // PTS = (now - old_base) ≈ 12.3 s instead of ≈ 0.  That is a problem
    // because the reconnect builds a BRAND-NEW media (factory reusable=FALSE):
    // fresh rtpbin / rtpsession / sinks whose pipeline running-time starts at
    // 0.  Feeding a buffer stamped at 12.3 s into a pipeline that just started
    // its clock makes the sink think the frame is already 12.3 s late, which
    // triggers "Can't determine running time" / late-buffer handling and can
    // delay or drop the first RTP packets on reconnect.
    //
    // Clearing it here forces nativePushEncodedBuffer() to re-arm the base on
    // its first push after the next on_media_configure (it logs "PTS base set
    // to <ns>"), so every new connection restarts its timeline near zero,
    // perfectly aligned with the fresh pipeline's clock.
    //
    // s_last_video_pts_ns and s_av_offset_ema belong to the SAME timeline
    // (they track video PTS and the audio-vs-video offset EMA), so they are
    // reset together – otherwise the A/V offset estimator would carry a stale
    // 12 s video PTS into the new session and mis-correct the first audio
    // frames until the EMA re-converged.
    s_pts_base          = GST_CLOCK_TIME_NONE;
    s_last_video_pts_ns = GST_CLOCK_TIME_NONE;
    s_av_offset_ema     = GST_CLOCK_TIME_NONE;

    // Reset the periodic diagnostic counters for BOTH video and audio so the
    // per-session log cadence (video % 90, audio % 50 in nativePush*Buffer)
    // restarts from frame 1 of the next connection, matching the rewound
    // t=0 timeline above.  Kept symmetric with the reset in nativeGstPipelineInit.
    s_video_push_count  = 0;
    s_audio_push_count  = 0;
    pthread_mutex_unlock(&s_lock);
}

// ---------------------------------------------------------------------------
// pay0_add_rtcp_fb_probe – pad probe installed on the payloader (pay0) src pad.
//
// The RTSP server builds the SDP from the caps negotiated on the payloader's
// src pad.  By appending the rtcp-fb-* fields to the outgoing CAPS event we
// make the server advertise the matching "a=rtcp-fb:<pt> ..." attributes, which
// tells the client it may send NACK (retransmission) / PLI / FIR feedback:
//
//     application/x-rtp, rtcp-fb-nack=(boolean)true,
//                        rtcp-fb-nack-pli=(boolean)true,
//                        rtcp-fb-nack-fir=(boolean)true
//
// The probe rewrites the first CAPS event and then removes itself, since caps
// are negotiated once per media.
// ---------------------------------------------------------------------------
static GstPadProbeReturn
pay0_add_rtcp_fb_probe(GstPad *pad, GstPadProbeInfo *info, gpointer /*user_data*/)
{
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (!event || GST_EVENT_TYPE(event) != GST_EVENT_CAPS)
        return GST_PAD_PROBE_OK;

    GstCaps *caps = NULL;
    gst_event_parse_caps(event, &caps);
    if (!caps)
        return GST_PAD_PROBE_OK;

    // Copy the negotiated caps and add the RTCP feedback fields.
    GstCaps *new_caps = gst_caps_copy(caps);
    gst_caps_set_simple(new_caps,
                        "rtcp-fb-nack",     G_TYPE_BOOLEAN, TRUE,
                        "rtcp-fb-nack-pli", G_TYPE_BOOLEAN, TRUE,
                        "rtcp-fb-nack-fir", G_TYPE_BOOLEAN, TRUE,
                        NULL);

    // Swap the in-flight CAPS event for one carrying the augmented caps so the
    // downstream peer (rtpbin) negotiates with the feedback fields present.
    GstEvent *new_event = gst_event_new_caps(new_caps);
    gst_caps_unref(new_caps);
    gst_event_unref(event);
    GST_PAD_PROBE_INFO_DATA(info) = new_event;

    // CRITICAL: also overwrite the sticky CAPS event already cached on THIS src
    // pad.  gst_pad_push_event() stores the original event as sticky BEFORE this
    // probe runs, so without this the payloader's own src pad would still report
    // the un-augmented caps.  The RTSP server builds the SDP from the payloader
    // src pad's caps (via gst_pad_get_current_caps()/notify::caps), so updating
    // the sticky event here is what makes the a=rtcp-fb attributes appear in the
    // SDP.  store_sticky takes its own ref; new_event's ref stays owned by info.
    gst_pad_store_sticky_event(pad, new_event);

    LOGI("pay0_add_rtcp_fb_probe: added rtcp-fb-nack/pli/fir to pay0 caps");

    // Caps are negotiated once; no need to keep the probe.
    return GST_PAD_PROBE_REMOVE;
}

// ---------------------------------------------------------------------------
// CresRTSPMedia – GstRTSPMedia subclass that injects the RTCP feedback
// (a=rtcp-fb) attributes directly into the generated SDP.
//
// Augmenting the payloader (pay0) src-pad caps in pay0_add_rtcp_fb_probe does
// NOT surface in the SDP on this GStreamer build: the server's SDP generator
// reads its caps from an internal rtpbin/session pad rather than the payloader
// src pad we modify, so the a=rtcp-fb lines never appear.  This GStreamer
// version's GstRTSPMediaFactoryClass has no create_sdp vfunc, but
// GstRTSPMediaClass exposes a setup_sdp vfunc.  Overriding setup_sdp lets us
// chain up to build the standard SDP, then post-process the finished
// GstSDPMessage and add the attributes deterministically to every
// feedback-capable (AVPF / SAVPF) video m= section, independent of how the
// caps are negotiated.  The factory is told to instantiate this subtype via
// gst_rtsp_media_factory_set_media_gtype().
// ---------------------------------------------------------------------------
#define CRES_TYPE_RTSP_MEDIA (cres_rtsp_media_get_type())
G_DECLARE_FINAL_TYPE(CresRTSPMedia, cres_rtsp_media,
                     CRES, RTSP_MEDIA, GstRTSPMedia)

struct _CresRTSPMedia {
    GstRTSPMedia parent;
};

G_DEFINE_TYPE(CresRTSPMedia, cres_rtsp_media, GST_TYPE_RTSP_MEDIA)

static gboolean
cres_rtsp_media_setup_sdp(GstRTSPMedia  *media,
                          GstSDPMessage *sdp,
                          GstSDPInfo    *info)
{
    // Build the standard SDP via the base class first.
    if (!GST_RTSP_MEDIA_CLASS(cres_rtsp_media_parent_class)
             ->setup_sdp(media, sdp, info))
        return FALSE;

    guint n_media = gst_sdp_message_medias_len(sdp);
    for (guint i = 0; i < n_media; i++) {
        // gst_sdp_message_get_media() returns a const pointer; cast away const
        // to append attributes (the message is the one we just populated).
        GstSDPMedia *m     = (GstSDPMedia *) gst_sdp_message_get_media(sdp, i);
        const gchar *mname = gst_sdp_media_get_media(m);
        const gchar *proto = gst_sdp_media_get_proto(m);

        // Only video, and only on a feedback-capable profile (AVPF / SAVPF).
        // rtcp-fb is illegal on a plain RTP/AVP m= section.  The factory is
        // restricted to the AVPF profile only (see nativeGstPipelineInit), so
        // in practice there is a single AVPF video m= section here; this guard
        // simply makes the pass robust if the profile set ever changes.
        if (g_strcmp0(mname, "video") != 0)
            continue;
        if (!proto || !strstr(proto, "AVPF"))
            continue;

        // Derive the dynamic payload type from the m= format list (first fmt);
        // fall back to "96", which is what the pipeline configures (pt=96).
        const gchar *pt = gst_sdp_media_get_format(m, 0);
        if (!pt || !*pt)
            pt = "96";

        gchar *nack     = g_strdup_printf("%s nack",     pt);
        gchar *nack_pli = g_strdup_printf("%s nack pli", pt);
        gchar *nack_fir = g_strdup_printf("%s nack fir", pt);

        gst_sdp_media_add_attribute(m, "rtcp-fb", nack);
        gst_sdp_media_add_attribute(m, "rtcp-fb", nack_pli);
        gst_sdp_media_add_attribute(m, "rtcp-fb", nack_fir);

        g_free(nack);
        g_free(nack_pli);
        g_free(nack_fir);

        LOGI("setup_sdp: injected a=rtcp-fb nack/pli/fir into AVPF video m= (pt=%s)", pt);
    }

    return TRUE;
}

static void
cres_rtsp_media_class_init(CresRTSPMediaClass *klass)
{
    GST_RTSP_MEDIA_CLASS(klass)->setup_sdp = cres_rtsp_media_setup_sdp;
}

static void
cres_rtsp_media_init(CresRTSPMedia * /*self*/)
{
}

// ---------------------------------------------------------------------------
// on_media_configure – called by the RTSP server when a new media pipeline
// is created (once, since the factory is shared).  Retrieves the appsrc
// element named "src" and stores it in s_appsrc so that
// nativePushEncodedBuffer() can start feeding encoded frames.
// ---------------------------------------------------------------------------
static void on_media_configure(GstRTSPMediaFactory * /*factory*/,
                                GstRTSPMedia        *media,
                                gpointer             /*user_data*/)
{
    LOGI("on_media_configure: RTSP media pipeline being configured");

    GstElement *bin = gst_rtsp_media_get_element(media);
    if (!bin) {
        LOGE("on_media_configure: gst_rtsp_media_get_element returned NULL");
        return;
    }

    // Fix timing issue for reconnecting clients: force the pipeline to use the
    // GStreamer system clock.  NOTE: the buffer PTSs we push are normalised to
    // s_pts_base (running-time relative to the first frame), so the pipeline
    // clock's absolute domain need not match the capture clock.  (The capture
    // clock that matters for A/V sync is audio-vs-video, and BOTH are
    // CLOCK_BOOTTIME — verified via SENSOR_INFO_TIMESTAMP_SOURCE=1.)  Without
    // this, a reused shared pipeline may inherit a stale or NULL clock after
    // a client disconnects, causing gst_rtsp_media_prepare() to fail with
    // "media was not prepared".
    // NOTE: gst_rtsp_media_get_element() returns a GstBin, not a GstPipeline,
    // in the GStreamer RTSP server version used here.  gst_pipeline_use_clock()
    // asserts GST_IS_PIPELINE and fails on a plain GstBin.  Use the base-class
    // gst_element_set_clock() which works on any GstElement.
    GstClock *sysclock = gst_system_clock_obtain();
    gst_element_set_clock(bin, sysclock);
    gst_object_unref(sysclock);  // bin now holds its own reference
    LOGI("on_media_configure: forced system clock on bin %p", bin);

    // NOTE: bus message handlers (error/warning/info/latency) are intentionally
    // NOT connected here.  At configure time the media is not prepared yet, so
    // gst_element_get_bus(bin) returns the bin's OWN private bus – which never
    // receives the messages that elements post once the bin is parented into
    // GstRTSPMedia's internal pipeline.  They are connected in on_media_prepared
    // on the real pipeline bus instead (see on_bus_message / on_media_prepared).

    // Read the stream mode committed during nativeGstPipelineInit.
    pthread_mutex_lock(&s_lock);
    StreamMode cur_mode = s_stream_mode;
    pthread_mutex_unlock(&s_lock);

    // Retrieve the video appsrc only when the pipeline has a video branch.
    GstElement *appsrc = NULL;
    if (cur_mode != STREAM_AUDIO_ONLY) {
        appsrc = gst_bin_get_by_name_recurse_up(GST_BIN(bin), "src");
        if (!appsrc) {
            LOGE("on_media_configure: no 'src' appsrc found (mode=%d)", (int)cur_mode);
            gst_object_unref(bin);
            return;
        }
        // GStreamer renames elements when they are added to a bin to avoid name
        // collisions (e.g. "src" → "appsrc0").  Log the actual name so we can
        // verify the correct element is being configured.
        LOGI("on_media_configure: video appsrc found: %p  actual-name='%s'",
             appsrc, GST_ELEMENT_NAME(appsrc));

        if (s_enable_rtcp_feedback) {
            // Advertise RTCP feedback (NACK / PLI / FIR) in the SDP by augmenting
            // the H.264/H.265 payloader's output caps.  These feedback messages are
            // only meaningful for video RTP.  Although this block already runs only
            // when cur_mode != STREAM_AUDIO_ONLY, guard on the element factory name
            // as well so we never touch an audio payloader (e.g. rtpmp4apay) that
            // happens to be named "pay0".
            GstElement *pay0 = gst_bin_get_by_name_recurse_up(GST_BIN(bin), "pay0");
            if (pay0) {
                GstElementFactory *pay_factory = gst_element_get_factory(pay0); // no ref added
                const gchar       *pay_fname   = pay_factory ? GST_OBJECT_NAME(pay_factory) : "";
                if (g_strcmp0(pay_fname, "rtph264pay") == 0 ||
                    g_strcmp0(pay_fname, "rtph265pay") == 0) {
                    GstPad *paysrc = gst_element_get_static_pad(pay0, "src");
                    if (paysrc) {
                        gst_pad_add_probe(paysrc, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                                        pay0_add_rtcp_fb_probe, NULL, NULL);
                        gst_object_unref(paysrc);
                        LOGI("on_media_configure: rtcp-fb caps probe installed on %s ('%s')",
                            pay_fname, GST_ELEMENT_NAME(pay0));
                    } else {
                        LOGE("on_media_configure: pay0 has no static src pad");
                    }
                } else {
                    LOGI("on_media_configure: pay0 is '%s' (not H.264/H.265) – rtcp-fb not set",
                        pay_fname);
                }
                gst_object_unref(pay0);
            } else {
                LOGW("on_media_configure: pay0 payloader not found; rtcp-fb not set");
            }
        } // end if (s_enable_rtcp_feedback)
    }
    gst_object_unref(bin);

    if (appsrc) {
    // Override appsrc properties for live streaming.
    // is-live=TRUE    : signals a real-time source so GStreamer computes latency
    //                   correctly and does not block in preroll.
    // do-timestamp=TRUE: enables GstBaseSrc live-clock machinery; for push mode
    //                   this does not override explicitly set PTS values (only
    //                   stamps buffers whose PTS is GST_CLOCK_TIME_NONE) but
    //                   ensures the element properly participates in latency queries.
    // min-latency=0   : appsrc reports 0 as its minimum latency in
    //                   GST_QUERY_LATENCY.  Since we supply accurate MediaCodec
    //                   PTS values the source introduces no additional delay; this
    //                   prevents gst_bin_recalculate_latency() from adding an
    //                   unnecessary offset to the pipeline's configured latency.
    // max-latency=0   : appsrc declares it will never buffer more than 0 ns of
    //                   data, allowing downstream elements to minimise their own
    //                   jitter-buffer depths.
    g_object_set(G_OBJECT(appsrc),
                 "is-live",      TRUE,
                 "do-timestamp", TRUE,   // fallback for CSD buffers; explicit PTS set below
                 "format",       GST_FORMAT_TIME,
                 "min-latency",  (gint64)0,
                 "max-latency",  (gint64)0,
                 // Disable the byte-count queue limit.  Default (200 KB) is smaller
                 // than one all-intra IDR at 100 Mbps/30 fps (~415 KB), causing
                 // enough-data to fire after every single frame and stalling the
                 // push path.  With max-bytes=0 flow-control is driven purely by
                 // need-data/enough-data backpressure from the payloader/network.
                 "max-bytes",    (guint64)0,
                 NULL);

    // Set Annex-B caps on the appsrc so that downstream parsers can negotiate
    // and the pipeline can complete its PAUSED state change (preroll).  Without
    // explicit caps the state change returns GST_STATE_CHANGE_FAILURE and the
    // RTSP server reports "failed to preroll pipeline".
    pthread_mutex_lock(&s_lock);
    CodecType codec = s_codec_type;
    pthread_mutex_unlock(&s_lock);

    GstCaps *appsrc_caps;
    if (codec == CODEC_H265) {
        appsrc_caps = gst_caps_new_simple("video/x-h265",
                "stream-format", G_TYPE_STRING, "byte-stream",
                "alignment",     G_TYPE_STRING, "au",
                NULL);
    } else {
        appsrc_caps = gst_caps_new_simple("video/x-h264",
                "stream-format", G_TYPE_STRING, "byte-stream",
                "alignment",     G_TYPE_STRING, "au",
                NULL);
    }
    g_object_set(G_OBJECT(appsrc), "caps", appsrc_caps, NULL);
    gst_caps_unref(appsrc_caps);
    LOGI("on_media_configure: appsrc caps set (%s Annex-B)",
         codec == CODEC_H265 ? "H.265" : "H.264");

    // Log ts-offset on the video appsrc.  ts-offset (GstBaseSrc property)
    // adds a fixed nanosecond offset to all outgoing buffer timestamps.
    // It is NOT explicitly set in this code, so the value should be 0 (default).
    // (appsrc is a source element and has no 'sync' property; sync/ts-offset
    //  on the network sink elements are logged in on_media_prepared.)
    {
        gint64 ts_offset_val = 0;
        g_object_get(G_OBJECT(appsrc), "ts-offset", &ts_offset_val, NULL);
        LOGI("on_media_configure: video appsrc ts-offset=%" G_GINT64_FORMAT " ns (not explicitly set; default=0)",
             ts_offset_val);
    }

    // Flow-control signals: only push frames when appsrc signals it needs
    // more data.  Mirrors the need-data/enough-data pattern in
    // cresStreamOutManager::media_configure.
    g_signal_connect(appsrc, "need-data",   G_CALLBACK(cb_src_need_data),   NULL);
    g_signal_connect(appsrc, "enough-data", G_CALLBACK(cb_src_enough_data), NULL);
    LOGI("on_media_configure: need-data/enough-data signals connected");
    } // end if (appsrc) – video-specific configuration

    // Media must be NON-reusable.
    //
    // Why: when the last client disconnects, GstRTSPMedia unprepares the media
    // and drives the media bin to NULL.  The appsrc inside it then stops and
    // its srcpad goes FLUSHING, so every gst_app_src_push_buffer() returns
    // GST_FLOW_FLUSHING (-2).
    //
    // With reusable=TRUE + shared=TRUE the server KEEPS that same (now NULL /
    // flushing) media cached and reuses it on the next client WITHOUT
    // reconstructing it.  Consequently on_media_configure never fires again,
    // s_appsrc keeps pointing at the dead/flushing appsrc, and the reconnecting
    // client gets no video forever (observed: push FAILED flow=-2 GST_FLOW_FLUSHING
    // with no "on_media_configure" log after the reconnect).
    //
    // Setting reusable=FALSE makes the server DESTROY the media on disconnect and
    // CONSTRUCT a fresh one on the next client.  That fires on_media_configure
    // again, which re-acquires the live appsrc and refreshes s_appsrc (the old
    // dead ref is unref'd just below).  Pushes resume on the new appsrc as soon
    // as the new media reaches PLAYING.  The brief gap between disconnect and the
    // next on_media_configure simply drops a few frames (harmless – an IDR is
    // re-injected via s_request_keyframe when the new media is configured).
    gst_rtsp_media_set_reusable(media, FALSE);

    pthread_mutex_lock(&s_lock);
    if (appsrc) {
        if (s_appsrc) gst_object_unref(s_appsrc);
        s_appsrc = appsrc;  // owns this ref (from gst_bin_get_by_name_recurse_up)
    }
    // A fresh media is being built.  Reset the diagnostic prepared flag; it is
    // set true again when on_media_prepared fires.  NOTE: this flag is NOT a
    // push gate – nativePushEncodedBuffer() gates solely on s_appsrc being
    // non-NULL (on_media_unprepared nulls it on teardown).  The brief
    // configure→prepared window only yields a few harmless GST_FLOW_FLUSHING
    // (-2) drops, which is preferable to gating on an unreliable signal.
    s_media_prepared = false;
    // Own s_need_data here (the new appsrc was just installed).  Prime it true
    // so the first frames flow as soon as the media is prepared; the appsrc's
    // own need-data/enough-data signals take over from this point.  (This reset
    // used to live in on_client_connected, which runs before s_appsrc exists.)
    s_need_data = true;
    // Always (re-)set s_request_keyframe here.
    //
    // The sequence for a first connect is:
    //   on_client_connected (DESCRIBE) → s_request_keyframe=true
    //   handleEncoderOutput            → consumes flag, re-injects CSD into
    //                                    nativePushEncodedBuffer → but s_appsrc
    //                                    is still NULL at that point → CSD is
    //                                    silently dropped → h264parse never
    //                                    receives SPS/PPS → drops every IDR with
    //                                    "broken/invalid nal" indefinitely.
    //   on_media_configure (SETUP)     → s_appsrc finally becomes valid.
    //
    // Setting the flag here ensures Java performs a fresh CSD + IDR injection
    // AFTER the appsrc is ready to accept buffers.  A second CSD injection is
    // completely harmless; a missing one causes an unrecoverable parse failure.
    s_request_keyframe = true;

    // Retrieve and configure the audio appsrc only when the pipeline has an audio branch.
    if (cur_mode != STREAM_VIDEO_ONLY) {
        GstElement *media_bin2  = gst_rtsp_media_get_element(media);
        GstElement *audio_appsrc = media_bin2
            ? gst_bin_get_by_name_recurse_up(GST_BIN(media_bin2), "audio")
            : NULL;
        // Opus path only: grab the named opusenc element so we can attach the
        // encode-latency pad probes below.  Fetched from the same media bin
        // before it is unref'd.
        GstElement *opus_enc = (media_bin2 && s_audio_raw_pcm)
            ? gst_bin_get_by_name_recurse_up(GST_BIN(media_bin2), "aenc")
            : NULL;
        if (media_bin2) gst_object_unref(media_bin2);
        if (audio_appsrc) {
            g_object_set(G_OBJECT(audio_appsrc),
                    "is-live",      TRUE,
                    "do-timestamp", TRUE,
                    "format",       GST_FORMAT_TIME,
                    "min-latency",  (gint64)0,
                    "max-latency",  (gint64)0,
                    "max-bytes",    (guint64)0,
                    NULL);
            // Set AAC caps for hardware-encoded AAC input (audio/mpeg, mpegversion=4,
            // stream-format=raw).  rate/channels come from s_audio_sample_rate /
            // s_audio_channels (detected from the pipeline string in
            // nativeGstPipelineInit) so they match whatever Java captured/encoded –
            // e.g. 44100 mono or 48000 stereo.  This native caps-set is required for
            // preroll and OVERRIDES the launch-string caps, so it must track the
            // runtime format.
            // aacparse requires codec_data (AudioSpecificConfig, ASC) in caps when
            // stream-format=raw; without it gst_aac_parse_sink_setcaps() returns
            // FALSE → not-negotiated → pipeline fails.
            //
            // s_lock is already held here (see the lock above), so read the
            // detected format directly without re-locking.
            int a_rate = s_audio_sample_rate;
            int a_ch   = s_audio_channels;

            if (s_audio_raw_pcm) {
                // Opus path: the appsrc carries raw PCM (audio/x-raw); the opusenc
                // element in the pipeline does the encoding, so no codec_data is
                // needed.  Like the AAC branch this OVERRIDES the launch-string caps
                // and is required for preroll, so it must track the runtime format.
                GstCaps *acaps = gst_caps_new_simple("audio/x-raw",
                    "format",   G_TYPE_STRING, "S16LE",
                    "layout",   G_TYPE_STRING, "interleaved",
                    "channels", G_TYPE_INT,    a_ch,
                    "rate",     G_TYPE_INT,    a_rate,
                    NULL);
                g_object_set(G_OBJECT(audio_appsrc), "caps", acaps, NULL);
                gst_caps_unref(acaps);
                LOGI("on_media_configure: audio appsrc caps = audio/x-raw S16LE "
                    "%d Hz %d ch (Opus path)", a_rate, a_ch);
            } else {
                // Build the 2-byte ASC for AAC-LC (audioObjectType=2):
                //   bit layout: objectType(5) | freqIdx(4) | channels(4) | 0(3) = 16 bits.
                //   e.g. 44100/mono=0x1208, 48000/stereo=0x1190, 48000/mono=0x1188.
                int a_freq_idx = aac_freq_index(a_rate);
                if (a_freq_idx < 0) {
                    LOGW("on_media_configure: unsupported audio rate %d – ASC falls back to 44100", a_rate);
                    a_freq_idx = 4;  // 44100
                }
                guint16 asc16 = (guint16)((2 << 11) | (a_freq_idx << 7) | (a_ch << 3));
                guint8 aac_asc[2] = { (guint8)(asc16 >> 8), (guint8)(asc16 & 0xFF) };
                GstBuffer *asc_buf = gst_buffer_new_allocate(NULL, sizeof(aac_asc), NULL);
                GstMapInfo asc_map;
                gst_buffer_map(asc_buf, &asc_map, GST_MAP_WRITE);
                memcpy(asc_map.data, aac_asc, sizeof(aac_asc));
                gst_buffer_unmap(asc_buf, &asc_map);

                GstCaps *acaps = gst_caps_new_simple("audio/mpeg",
                    "mpegversion",   G_TYPE_INT,    4,
                    "stream-format", G_TYPE_STRING, "raw",
                    "channels",      G_TYPE_INT,    a_ch,
                    "rate",          G_TYPE_INT,    a_rate,
                    NULL);
                gst_caps_set_simple(acaps, "codec_data", GST_TYPE_BUFFER, asc_buf, NULL);
                gst_buffer_unref(asc_buf);  // caps holds its own ref
                g_object_set(G_OBJECT(audio_appsrc), "caps", acaps, NULL);
                gst_caps_unref(acaps);
                LOGI("on_media_configure: audio appsrc configured for AAC (hardware-encoded) %d Hz %s (codec_data=0x%02x%02x)",
                    a_rate, a_ch == 2 ? "stereo" : "mono", aac_asc[0], aac_asc[1]);
            }  // end if (s_audio_raw_pcm) / else AAC
            g_signal_connect(audio_appsrc, "need-data",   G_CALLBACK(cb_audio_need_data),   NULL);
            g_signal_connect(audio_appsrc, "enough-data", G_CALLBACK(cb_audio_enough_data), NULL);
            if (s_audio_appsrc) gst_object_unref(s_audio_appsrc);
            s_audio_appsrc = audio_appsrc;

            // Log ts-offset on the audio appsrc (same reasoning as video appsrc above).
            {
                gint64 ts_offset_val = 0;
                g_object_get(G_OBJECT(audio_appsrc), "ts-offset", &ts_offset_val, NULL);
                LOGI("on_media_configure: audio appsrc ts-offset=%" G_GINT64_FORMAT " ns (not explicitly set; default=0)",
                    ts_offset_val);
            }

            // Opus path: attach sink+src buffer probes to opusenc to measure the
            // encode compute that the Java "Opus push latency" number excludes.
            if (opus_enc) {
                opus_probe_reset();
                GstPad *enc_sink = gst_element_get_static_pad(opus_enc, "sink");
                GstPad *enc_src  = gst_element_get_static_pad(opus_enc, "src");
                if (enc_sink) {
                    gst_pad_add_probe(enc_sink, GST_PAD_PROBE_TYPE_BUFFER,
                                    opus_enc_sink_probe, NULL, NULL);
                    gst_object_unref(enc_sink);
                }
                if (enc_src) {
                    gst_pad_add_probe(enc_src, GST_PAD_PROBE_TYPE_BUFFER,
                                    opus_enc_src_probe, NULL, NULL);
                    gst_object_unref(enc_src);
                }
                LOGI("on_media_configure: attached opusenc encode-latency probes (aenc)");
            }
        } else {
            LOGW("on_media_configure: no 'audio' appsrc found – audio stream disabled");
        }
        if (opus_enc) gst_object_unref(opus_enc);
    } // end if (cur_mode != STREAM_VIDEO_ONLY)
    pthread_mutex_unlock(&s_lock);

    // Connect to the 'prepared' signal purely as a diagnostic marker (it sets
    // s_media_prepared for log tracing).  The latency recalculation is NOT done
    // there – it runs at the end of on_client_play_request, once the transport
    // sinks are joined.  See on_media_prepared / on_client_play_request above.
    g_signal_connect(media, "prepared", G_CALLBACK(on_media_prepared), NULL);

    // Connect to the 'unprepared' signal so we stop pushing into the appsrc
    // the instant the media is torn down (last client disconnected).  The
    // handler nulls s_appsrc/s_audio_appsrc, after which nativePushEncodedBuffer
    // drops frames cleanly via its `if (!s_appsrc) return;` guard instead of
    // pushing into a flushing element and logging flow=-2 repeatedly.
    g_signal_connect(media, "unprepared", G_CALLBACK(on_media_unprepared), NULL);

    LOGI("on_media_configure: appsrc ready - keyframe requested for new client");
}

// ---------------------------------------------------------------------------
// on_client_play_request – fired by GstRTSPClient just before the server
// sends its 200 PLAY response, after every SETUP in this session has been
// processed.
//
// At SETUP time GstRTSPServer calls gst_rtsp_stream_join_bin(), which inserts
// the transport-specific network sink elements into the shared media bin:
//   TCP mode : multifdsink (or a per-connection internal sink, depending on
//              GStreamer version)
//   UDP mode : udpsink (one per stream; two per stream for RTP + RTCP)
//
// This is therefore the earliest reliable point to read ts-offset and sync on
// the actual network sinks.  Both properties are inherited from GstBaseSink:
//   ts-offset  – nanosecond timestamp adjustment applied before clock-sync
//                comparison.  Default = 0 (no shift).
//   sync       – whether the sink blocks on the pipeline clock to pace output.
//                Default = TRUE for most sinks; FALSE for multifdsink.
// ---------------------------------------------------------------------------
static void on_client_play_request(GstRTSPClient  * /*client*/,
                                   GstRTSPContext *ctx,
                                   gpointer        /*user_data*/)
{
    if (!ctx || !ctx->media) {
        LOGW("on_client_play_request: no media in context - cannot scan sinks");
        return;
    }

#ifdef USE_RTSP_LOWER_TRANS_TCP_ONLY
    pthread_mutex_lock(&s_lock);
    TransportMode transport = s_transport_mode;
    pthread_mutex_unlock(&s_lock);


    LOGI("on_client_play_request: scanning sink properties (configured transport=%s)",
         transport == TRANSPORT_UDP ? "UDP" : "TCP");
#endif

    // gst_rtsp_media_get_element() returns only the USER sub-bin – the elements
    // from the launch string (appsrc, parser, payloader).  The rtpbin and
    // transport sinks (udpsink for UDP, multifdsink/appsink for TCP) are added
    // to the PARENT GstPipeline of that sub-bin by gst_rtsp_stream_join_bin()
    // during SETUP processing, which completes before "play-request" fires.
    // Obtaining the parent pipeline via gst_object_get_parent() exposes all
    // elements, including the network sinks whose ts-offset/sync we need.
    GstElement *user_bin = gst_rtsp_media_get_element(ctx->media);
    if (!user_bin) {
        LOGW("on_client_play_request: gst_rtsp_media_get_element returned NULL");
        return;
    }

    GstObject *full_pipeline_obj = gst_object_get_parent(GST_OBJECT(user_bin));
    gst_object_unref(user_bin);   // release sub-bin ref; we have the parent now

    if (!full_pipeline_obj || !GST_IS_BIN(full_pipeline_obj)) {

#ifdef USE_RTSP_LOWER_TRANS_TCP_ONLY
        LOGW("on_client_play_request: user sub-bin has no parent pipeline - "
             "network sinks not accessible (transport=%s)",
             transport == TRANSPORT_UDP ? "UDP" : "TCP");
#else
        LOGW("on_client_play_request: user sub-bin has no parent pipeline - "
                "network sinks not accessible");
#endif

        if (full_pipeline_obj) gst_object_unref(full_pipeline_obj);
        return;
    }

    GstBin *full_pipeline = GST_BIN(full_pipeline_obj);
    LOGI("on_client_play_request: scanning full pipeline[%p] '%s'",
         full_pipeline, GST_ELEMENT_NAME(full_pipeline));

    // Cache the full pipeline for the periodic element-dump timer.
    //
    // OWNERSHIP: this is the RTSP media's internal GstPipeline.  We take ONE ref
    // here and release it in on_media_unprepared when the media is torn down
    // (symmetric with how s_appsrc/s_audio_appsrc are released there).  The
    // factory is shared, so play-request can fire for several clients on the
    // SAME media/pipeline – the `!= full_pipeline` guard makes us ref only the
    // first time so we never leak one ref per client.  Because the GMainLoop
    // thread serialises events (unprepared(old) -> configure(new) ->
    // play-request(new)), s_full_pipeline is always NULL or == full_pipeline
    // here – never a different stale pipeline – so no unref is needed in this path.
    //
    // LOCKING: s_full_pipeline is read+ref'd under s_lock by nativeDumpPipelineNow
    // / Cam2Streamer_RefFullPipeline and written under s_lock by
    // on_media_unprepared / nativeGstPipelineDestroy, so this write also holds s_lock.
    pthread_mutex_lock(&s_lock);
    if (s_full_pipeline != full_pipeline) {
        s_full_pipeline = GST_BIN(gst_object_ref(full_pipeline_obj));
        LOGI("on_client_play_request: full pipeline cached for periodic dump");
    } else {
        LOGI("on_client_play_request: full pipeline already cached (same media)");
    }
    pthread_mutex_unlock(&s_lock);

    GstIterator *it = gst_bin_iterate_recurse(full_pipeline);
    if (it) {
        GValue item = G_VALUE_INIT;
        GstIteratorResult res;
        gboolean done       = FALSE;
        int      total      = 0;
        int      with_props = 0;
        while (!done) {
            res = gst_iterator_next(it, &item);
            switch (res) {
                case GST_ITERATOR_OK: {
                    GstElement *elem = GST_ELEMENT(g_value_get_object(&item));
                    if (elem) {
                        total++;
                        GObjectClass *klass         = G_OBJECT_GET_CLASS(elem);
                        gboolean      has_ts_offset = (g_object_class_find_property(klass, "ts-offset") != NULL);
                        gboolean      has_sync      = (g_object_class_find_property(klass, "sync")      != NULL);
                        if (has_ts_offset || has_sync) {
                            with_props++;
                            gint64   ts_off = 0;
                            gboolean sync_v = FALSE;
                            if (has_ts_offset) g_object_get(G_OBJECT(elem), "ts-offset", &ts_off, NULL);
                            if (has_sync)      g_object_get(G_OBJECT(elem), "sync",      &sync_v, NULL);
                            LOGI("on_client_play_request: [%s] (%s)  ts-offset=%" G_GINT64_FORMAT " ns  sync=%s (before override)",
                                 GST_ELEMENT_NAME(elem), G_OBJECT_TYPE_NAME(elem),
                                 has_ts_offset ? ts_off : (gint64)0,
                                 has_sync ? (sync_v ? "TRUE" : "FALSE") : "N/A");

                            // For a live RTSP transmit pipeline, sync=TRUE on an
                            // RTP/RTCP sink means GStreamer holds each packet until
                            // its running_time is reached on the sender's pipeline
                            // clock.  This adds sender-side clock-pacing latency on
                            // top of the encode path.  It is wrong for a transmitter:
                            //   - RTP timing is carried in the RTP timestamp header;
                            //     the receiver's jitter buffer enforces playout timing.
                            //   - We want to send packets as soon as they are ready.
                            //   - ts-offset=0 means no intentional shift is applied,
                            //     so leaving it at 0 is correct.
                            // Override sync to FALSE on every sink that has the property.
                            if (has_sync && sync_v) {
                                g_object_set(G_OBJECT(elem), "sync", FALSE, NULL);
                                LOGI("on_client_play_request: [%s] sync overridden TRUE→FALSE "
                                     "to eliminate sender-side clock-pacing latency",
                                     GST_ELEMENT_NAME(elem));
                            }

                            // Disable async state-change on sinks.  async=TRUE causes
                            // the sink to participate in preroll: it blocks pipeline
                            // state transitions until a buffer arrives.  For live RTSP
                            // transmit sinks (appsink) there is no preroll – frames
                            // arrive in real-time and must be forwarded immediately.
                            // Forcing async=FALSE eliminates that preroll-wait path.
                            if (g_object_class_find_property(klass, "async")) {
                                gboolean async_v = FALSE;
                                g_object_get(G_OBJECT(elem), "async", &async_v, NULL);
                                if (async_v) {
                                    g_object_set(G_OBJECT(elem), "async", FALSE, NULL);
                                    LOGI("on_client_play_request: [%s] async overridden TRUE→FALSE "
                                         "to skip preroll-wait on live sink",
                                         GST_ELEMENT_NAME(elem));
                                }
                            }

                            // Disable processing-deadline (GstBaseSink, default 20 ms).
                            // With a non-zero deadline the base-sink drops any frame
                            // whose upstream processing time exceeds the limit, silently
                            // discarding RTP packets during encoder spikes.  Setting
                            // it to 0 disables the deadline entirely.
                            if (g_object_class_find_property(klass, "processing-deadline")) {
                                g_object_set(G_OBJECT(elem), "processing-deadline", (guint64)0, NULL);
                                LOGI("on_client_play_request: [%s] processing-deadline set to 0 "
                                     "to prevent deadline-based packet drops",
                                     GST_ELEMENT_NAME(elem));
                            }
                        }
                    }
                    g_value_reset(&item);
                    break;
                }
                case GST_ITERATOR_RESYNC:
                    gst_iterator_resync(it);
                    break;
                case GST_ITERATOR_DONE:
                case GST_ITERATOR_ERROR:
                default:
                    done = TRUE;
                    break;
            }
        }
        g_value_unset(&item);
        gst_iterator_free(it);

#ifdef USE_RTSP_LOWER_TRANS_TCP_ONLY
        LOGI("on_client_play_request: scan complete - %d element(s) total, "
             "%d with ts-offset/sync (transport=%s)",
             total, with_props,
             transport == TRANSPORT_UDP ? "UDP" : "TCP");
#else
        LOGI("on_client_play_request: scan complete - %d element(s) total, "
             "%d with ts-offset/sync",
             total, with_props);
#endif
        // -----------------------------------------------------------------------
        // Pass 2: iterate again to configure rtpbin and rtpsession elements.
        //
        // WHY iterate instead of gst_bin_get_by_name("rtpbin0"):
        //   GstRTSPServer auto-names elements with a per-process counter, so the
        //   actual name in the pipeline is typically "rtpbin1", "rtpbin2", etc.
        //   (observed in log12: <rtpbin1>, <rtpsession2>, <rtpsession3>).
        //   Searching by the fixed string "rtpbin0" silently fails every time,
        //   leaving the sender rtpbin with its default latency (200 ms) and
        //   do-lost=TRUE.  We use type-based detection instead.
        //
        // RTCP SR NTP timestamp source:
        //   The default ntp-time-source=0 ("ntp") is RFC 3550-compliant:
        //   it calls g_get_real_time() (CLOCK_REALTIME) and adds 2208988800 s
        //   to convert from Unix epoch (1970) to NTP epoch (1900).
        //   ntp-time-source=1 ("unix") uses CLOCK_REALTIME WITHOUT the offset –
        //   this is NOT RFC 3550 and will confuse standards-compliant receivers.
        //   We explicitly set 0 here to guard against any default change.
        // -----------------------------------------------------------------------
        {
            GstIterator *it2 = gst_bin_iterate_recurse(full_pipeline);
            if (it2) {
                GValue item2 = G_VALUE_INIT;
                GstIteratorResult res2;
                gboolean done2 = FALSE;
                while (!done2) {
                    res2 = gst_iterator_next(it2, &item2);
                    switch (res2) {
                        case GST_ITERATOR_OK: {
                            GstElement *elem = GST_ELEMENT(g_value_get_object(&item2));
                            if (elem) {
                                GObjectClass *klass = G_OBJECT_GET_CLASS(elem);
                                const gchar  *tname = G_OBJECT_TYPE_NAME(elem);

                                // ---- GstRtpBin ----
                                if (g_str_has_prefix(tname, "GstRtpBin") ||
                                    g_object_class_find_property(klass, "do-lost")) {
                                    if (s_rtpbin_buffer_mode_none &&
                                        g_object_class_find_property(klass, "buffer-mode")) {
                                        g_object_set(G_OBJECT(elem),
                                                     "do-lost",     FALSE,
                                                     "buffer-mode", 0 /* none */,
                                                     "latency",     (guint)0,
                                                     NULL);
                                        LOGI("on_client_play_request: [%s] do-lost=FALSE"
                                             " buffer-mode=none(0) latency=0"
                                             " (rtpbin_buffer_mode_none=true)",
                                             GST_ELEMENT_NAME(elem));
                                    } else {
                                        g_object_set(G_OBJECT(elem),
                                                     "do-lost", FALSE,
                                                     "latency", (guint)0,
                                                     NULL);
                                        LOGI("on_client_play_request: [%s] do-lost=FALSE latency=0"
                                             " (buffer-mode left at default)",
                                             GST_ELEMENT_NAME(elem));
                                    }
                                }

                                // ---- GstRTPSession – confirm NTP source is RFC 3550-compliant ----
                                // ntp-time-source values (from gstrtpsession.c):
                                //   0 = "ntp"          → g_get_real_time() + 2208988800 s offset
                                //                         = CLOCK_REALTIME on 1900 NTP epoch  ← RFC 3550 ✓
                                //   1 = "unix"         → g_get_real_time() only, NO offset
                                //                         = CLOCK_REALTIME on 1970 Unix epoch ← NOT RFC 3550 ✗
                                //   2 = "running-time" → pipeline running time (relative)      ✗
                                //   3 = "clock-time"   → pipeline clock = CLOCK_MONOTONIC uptime ✗
                                //
                                // The default (0, "ntp") is already correct: it calls g_get_real_time()
                                // (CLOCK_REALTIME, real wall clock) and adds the Unix→NTP offset
                                // (2208988800 s) to produce proper RFC 3550 NTP timestamps.
                                // Explicitly set it to 0 to guard against any caller changing it.
                                if (g_object_class_find_property(klass, "ntp-time-source")) {
                                    g_object_set(G_OBJECT(elem),
                                                 "ntp-time-source", (guint)0 /* ntp, RFC 3550 */,
                                                 NULL);
                                    LOGI("on_client_play_request: [%s] ntp-time-source=ntp(0)"
                                         " (g_get_real_time + 2208988800s offset ="
                                         " correct RFC 3550 NTP epoch Jan 1 1900)",
                                         GST_ELEMENT_NAME(elem));
                                }

                                // ---- rtpstorage – disable retransmission buffer ----
                                if (g_object_class_find_property(klass, "size-time") &&
                                    g_str_has_prefix(tname, "GstRtpStorage")) {
                                    g_object_set(G_OBJECT(elem),
                                                 "size-time", (guint64)0, NULL);
                                    LOGI("on_client_play_request: [%s] size-time=0"
                                         " (retransmission store disabled,"
                                         " do-retransmission=FALSE on sender)",
                                         GST_ELEMENT_NAME(elem));
                                }
                            }
                            g_value_reset(&item2);
                            break;
                        }
                        case GST_ITERATOR_RESYNC: gst_iterator_resync(it2); break;
                        case GST_ITERATOR_DONE:
                        case GST_ITERATOR_ERROR:
                        default: done2 = TRUE; break;
                    }
                }
                g_value_unset(&item2);
                gst_iterator_free(it2);
            }
        }
    } else {
        LOGW("on_client_play_request: gst_bin_iterate_recurse returned NULL");
    }

    // ---------------------------------------------------------------------------
    // Recalculate pipeline latency now that the FULL sink set exists.
    //
    // This is the correct point (not on_media_prepared): the transport sinks
    // (appsink0/appsink1) were joined by gst_rtsp_stream_join_bin() during the
    // SETUP exchanges that precede "play-request", and the pass above has just
    // set them to sync=FALSE / async=FALSE / processing-deadline=0.  Running
    // gst_bin_recalculate_latency() here distributes a clean GST_EVENT_LATENCY
    // with configured_latency=0 to rtpbin/rtpsession, eliminating the
    // "Can't determine running time without knowing configured latency" warning
    // on the first RTP packets.  (Called in on_media_prepared it ran too early,
    // against an incomplete sink set, and the bin logged "did not really
    // configure latency".)
    //
    // Run it only ONCE per media: "play-request" fires for every client, and
    // because the factory is shared=TRUE several clients can attach to the same
    // media – without this guard the recalc would run N times on the same
    // pipeline (harmless but wasteful).  The flag is stored on the media object
    // and dies with it, so the next connect's fresh media (reusable=FALSE)
    // recalculates again.
    // ---------------------------------------------------------------------------
    if (ctx->media &&
        !g_object_get_data(G_OBJECT(ctx->media), "latency-recalc-done")) {
        gst_bin_recalculate_latency(full_pipeline);
        g_object_set_data(G_OBJECT(ctx->media), "latency-recalc-done",
                          GINT_TO_POINTER(1));
        LOGI("on_client_play_request: pipeline latency recalculated (configured_latency=0)");
    } else {
        LOGI("on_client_play_request: latency already recalculated for this media – skipping");
    }

    gst_object_unref(full_pipeline_obj);
}

// ---------------------------------------------------------------------------
// on_client_closed – fired by GstRTSPClient when the client connection is
// closed (graceful TEARDOWN or transport drop).  Decrements the active client
// counter and logs it.  Note: the media teardown / appsrc cleanup is handled
// separately by on_media_unprepared; this callback only tracks the count.
// ---------------------------------------------------------------------------
static void on_client_closed(GstRTSPClient * /*client*/, gpointer /*user_data*/)
{
    pthread_mutex_lock(&s_lock);
    if (s_client_count > 0) s_client_count--;
    int count = s_client_count;
    pthread_mutex_unlock(&s_lock);
    LOGI("on_client_closed: RTSP client disconnected - active client count=%d", count);
}

// ---------------------------------------------------------------------------
// on_client_connected – fired by the RTSP server every time any client opens
// a connection (OPTIONS / DESCRIBE / SETUP / PLAY sequence).
//
// Sets s_request_keyframe so that handleEncoderOutput() in Java will
// immediately request an IDR from the encoder and re-inject the cached CSD
// (VPS/SPS/PPS).  This is necessary for clients that connect after the
// initial on_media_configure has already fired: with
// GST_RTSP_SUSPEND_MODE_NONE the pipeline never suspends, so
// on_media_configure will NOT fire again for subsequent clients.
// ---------------------------------------------------------------------------
static void on_client_connected(GstRTSPServer * /*server*/,
                                GstRTSPClient  *client,
                                gpointer        /*user_data*/)
{
    pthread_mutex_lock(&s_lock);
    s_request_keyframe = true;
    // Track the active client count.  s_need_data is intentionally NOT reset
    // here anymore: this callback fires on DESCRIBE, before on_media_configure
    // installs the new appsrc, so the reset belongs in on_media_configure
    // (which now owns it).
    s_client_count++;
    int count = s_client_count;

#ifdef USE_RTSP_LOWER_TRANS_TCP_ONLY
    TransportMode transport = s_transport_mode;
#endif

    pthread_mutex_unlock(&s_lock);
    LOGI("on_client_connected: RTSP client connected - active client count=%d, IDR keyframe requested", count);

    //6-08-2026: need to set this flag back to streamout app.
    streamout_status_fb(0, stream_status_client_connected, "RTSP client connected");

#ifdef USE_RTSP_LOWER_TRANS_TCP_ONLY
    // ---------------------------------------------------------------------------
    // TCP socket tuning for lowest-latency interleaved RTP transport.
    //
    // In TCP interleaved mode (RFC 2326 §10.12) the GstRTSPServer multiplexes
    // all RTP and RTCP traffic on the same TCP connection used for RTSP
    // signalling.  Two standard TCP mechanisms add latency on this path:
    //
    //  1. Nagle's algorithm (RFC 896): coalesces small writes into MSS-sized
    //     segments, holding partial segments for up to ~200 ms waiting for
    //     more data.  Every RTCP packet (~72 B) and the final fragment of
    //     each RTP frame is subject to this delay.  TCP_NODELAY disables
    //     Nagle: each write() is flushed to the network immediately.
    //
    //  2. Delayed ACK (RFC 1122 §4.2.3.2): the receiving kernel holds ACKs
    //     for up to 40 ms hoping to piggyback them on outbound data.  On the
    //     receiver side this delays the sender's congestion window advance.
    //     TCP_QUICKACK (Linux-specific) forces the kernel to ACK immediately.
    //     NOTE: TCP_QUICKACK resets to 0 after each recv() on some kernels;
    //     it is set here as a best-effort hint for the initial burst and for
    //     RTSP signalling ACKs.  The receiver must set it on their side to
    //     fully eliminate delayed ACKs on the data path.
    // ---------------------------------------------------------------------------
    if (transport == TRANSPORT_TCP && s_tcp_no_delay) {
        GstRTSPConnection *conn = gst_rtsp_client_get_connection(client);
        if (conn) {
            // Read and write share the same underlying fd for TCP interleaved.
            GSocket *sock = gst_rtsp_connection_get_write_socket(conn);
            if (sock) {
                int fd  = g_socket_get_fd(sock);
                int one = 1;

                // Disable Nagle – flush every write() immediately.
                if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0) {
                    LOGI("on_client_connected: TCP_NODELAY set on fd=%d"
                         " (Nagle disabled – each RTP write flushed immediately)", fd);
                } else {
                    LOGW("on_client_connected: TCP_NODELAY setsockopt failed errno=%d (%s)",
                         errno, strerror(errno));
                }

                // Request immediate ACKs (best-effort; resets after each recv).
#ifdef TCP_QUICKACK
                if (setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one)) == 0) {
                    LOGI("on_client_connected: TCP_QUICKACK set on fd=%d", fd);
                } else {
                    LOGW("on_client_connected: TCP_QUICKACK setsockopt failed errno=%d (%s)",
                         errno, strerror(errno));
                }
#endif

                // Shrink the socket send buffer so the kernel cannot queue
                // more than ~4 frames of stale data ahead of fresh frames.
                // Must be set BEFORE the socket is fully connected; setting
                // it here (immediately after accept) is the correct point.
                if (s_so_sndbuf_size > 0) {
                    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                                   &s_so_sndbuf_size, sizeof(s_so_sndbuf_size)) == 0) {
                        LOGI("on_client_connected: SO_SNDBUF set to %d bytes on fd=%d",
                             s_so_sndbuf_size, fd);
                    } else {
                        LOGW("on_client_connected: SO_SNDBUF setsockopt failed errno=%d (%s)",
                             errno, strerror(errno));
                    }
                }

                // Cap the kernel's unsent-data backlog.  When backlog exceeds
                // this limit the socket blocks GStreamer writes, applying
                // back-pressure and forcing old frames to be dropped rather
                // than delayed.  Requires Linux ≥ 4.4 (Android 7+).
#ifdef TCP_NOTSENT_LOWAT
                if (s_tcp_notsent_lowat > 0) {
                    if (setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT,
                                   &s_tcp_notsent_lowat, sizeof(s_tcp_notsent_lowat)) == 0) {
                        LOGI("on_client_connected: TCP_NOTSENT_LOWAT set to %d bytes on fd=%d",
                             s_tcp_notsent_lowat, fd);
                    } else {
                        LOGW("on_client_connected: TCP_NOTSENT_LOWAT setsockopt failed errno=%d (%s)",
                             errno, strerror(errno));
                    }
                }
#endif

                // Mark packets with DSCP Expedited Forwarding (EF, 0xb8 = 46<<2)
                // for priority treatment on managed switches and routers.
                if (s_ip_tos_dscp_ef) {
                    int tos = 0xb8; // DSCP EF (PHB 46, low latency / low loss)
                    if (setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) == 0) {
                        LOGI("on_client_connected: IP_TOS=0x%02x (DSCP EF) set on fd=%d", tos, fd);
                    } else {
                        LOGW("on_client_connected: IP_TOS setsockopt failed errno=%d (%s)",
                             errno, strerror(errno));
                    }
                }
            } else {
                LOGW("on_client_connected: could not get write socket from RTSP connection");
            }
        } else {
            LOGW("on_client_connected: gst_rtsp_client_get_connection returned NULL");
        }
    }
#endif

    // Connect play-request on this client so ts-offset/sync are logged after
    // all SETUP exchanges for this session have completed and the transport
    // sinks (udpsink / multifdsink) have been added to the media bin.
    g_signal_connect(client, "play-request", G_CALLBACK(on_client_play_request), NULL);

    // Connect "closed" so we decrement the active client counter when this
    // client disconnects (TEARDOWN or transport drop).
    g_signal_connect(client, "closed", G_CALLBACK(on_client_closed), NULL);
}

// ---------------------------------------------------------------------------// nativeGstPipelineInit
//   Java signature: static native boolean nativeGstPipelineInit(String pipeline, int port, int transport)
//
//   Creates a GstRTSPServer on 'port', registers a shared media factory
//   whose pipeline string is 'pipeline' (must end with an rtph264pay/rtph265pay
//   element named "pay0"), attaches the server to a new GMainContext, and
//   starts a GMainLoop thread.
//
//   'transport': 0 = TCP (RTP interleaved over RTSP control connection),
//                1 = UDP (RTP over separate UDP unicast sockets).
//
//   The appsrc element (named "src" in the pipeline) is not accessible until
//   the first RTSP client connects and triggers on_media_configure().
//   nativePushEncodedBuffer() silently drops frames until that point.
//
//   Returns JNI_TRUE on success, JNI_FALSE on any error.
// ---------------------------------------------------------------------------
extern "C" JNIEXPORT jboolean JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativeGstPipelineInit(
        JNIEnv *env, jclass /*clazz*/, jstring jpipeline, jint jport, jint jtransport)
{
    const char *pipeline_str = env->GetStringUTFChars(jpipeline, nullptr);
    if (!pipeline_str) {
        LOGE("nativeGstPipelineInit: GetStringUTFChars failed");
        return JNI_FALSE;
    }
    LOGI("nativeGstPipelineInit: port=%d  transport=%s  pipeline=%s",
         (int)jport,
         jtransport == TRANSPORT_UDP ? "UDP" : "TCP",
         pipeline_str);

    // Enable GStreamer debug output for appsrc, basesrc, and RTP elements.
    // On Android, the GST_DEBUG environment variable is not inherited from the
    // shell, so debug levels must be set programmatically.  GStreamer's Android
    // integration redirects all debug output to logcat under the tag "GStreamer".
    // Level 5 = LOG (very verbose; includes every buffer push, caps negotiation,
    // state change, and RTP packet timestamp decision).
    gst_debug_set_active(TRUE);
    // ---------------------------------------------------------------------------
    // GStreamer debug categories for A/V latency diagnosis.
    //
    // KEY CATEGORIES for understanding the ~280 ms extra latency in
    // VIDEO_AND_AUDIO_BOTH mode vs VIDEO_ONLY:
    //
    //  appsrc/basesrc:5  – buffer push path, PTS values stamped on each buffer.
    //                      Look for: "pushing buffer ... pts X" lines.
    //                      Compare video vs audio PTS; a large positive offset
    //                      (audio_pts >> video_pts) confirms A/V mismatch.
    //
    //  rtp*:5            – covers rtph264pay, rtph265pay, rtpmp4apay,
    //                      rtpsession, rtpbin, rtpjitterbuffer.
    //                      Look for: "buffer with pts" in rtpbin; any "sync"
    //                      or "wait" messages in rtpjitterbuffer indicate
    //                      sender-side buffering caused by A/V clock mismatch.
    //                      Also: RTCP SR "ntp/rtp" lines expose the NTP↔RTP
    //                      clock mapping that the RTSP client uses for A/V sync.
    //
    //  basesink:4        – sink scheduling: "rendering buffer at..." lines show
    //                      whether the sink is pacing output (sync=TRUE adds
    //                      sender-side hold); should be FALSE after our override.
    //
    //  queue:4           – audPostQ fill level; "filled ... buffers" shows if
    //                      the audio queue is building up (backlog = latency).
    //
    //  aacparse:5        – AAC frame timing; confirms codec_data negotiation.
    // ---------------------------------------------------------------------------
    /*
    gst_debug_set_threshold_from_string(
        "appsrc:5,basesrc:5,"       // video/audio appsrc push path + PTS stamping
        "rtp*:5,"                   // rtph264pay/rtph265pay/rtpmp4apay, rtpsession,
                                    //   rtpbin, rtpjitterbuffer – A/V sync decisions
        "basesink:4,"               // sink scheduling; verify sync=FALSE is honoured
        "queue:4,"                  // audPostQ fill level – non-zero = audio backlog
        "aacparse:5,"               // AAC frame parsing / caps negotiation
        "avdec_aac:5,"              // AAC decoder (if present in pipeline)
        "audioconvert:4,"           // PCM conversion (INFO level – less noisy)
        "audiotestsrc:4",           // any test audio sources
        FALSE);
    LOGI("nativeGstPipelineInit: GStreamer debug enabled: "
         "appsrc:5 basesrc:5 rtp*:5 basesink:4 queue:4 aacparse:5 avdec_aac:5 "
         "audioconvert:4 – search logcat tag GStreamer for A/V PTS offset");
    */
    // Detect codec type from pipeline string so on_media_configure can set
    // the correct Annex-B caps (needed for preroll cap negotiation).
    CodecType detected_codec =
        (strstr(pipeline_str, "h265") || strstr(pipeline_str, "hevc") ||
         strstr(pipeline_str, "H265") || strstr(pipeline_str, "HEVC"))
        ? CODEC_H265 : CODEC_H264;
    LOGI("nativeGstPipelineInit: detected codec %s",
         detected_codec == CODEC_H265 ? "H.265" : "H.264");

    // Detect stream mode from the presence of named appsrc elements in the
    // pipeline string: "name=src" for video, "name=audio" for audio.
    bool has_video_appsrc = (strstr(pipeline_str, "name=src")   != NULL);
    bool has_audio_appsrc = (strstr(pipeline_str, "name=audio") != NULL);
    StreamMode detected_mode = (has_video_appsrc && has_audio_appsrc)
            ? STREAM_VIDEO_AND_AUDIO
            : (has_video_appsrc ? STREAM_VIDEO_ONLY : STREAM_AUDIO_ONLY);
    LOGI("nativeGstPipelineInit: stream mode=%s",
         detected_mode == STREAM_VIDEO_AND_AUDIO ? "VIDEO_AND_AUDIO" :
         detected_mode == STREAM_VIDEO_ONLY       ? "VIDEO_ONLY" : "AUDIO_ONLY");

    // Detect the audio AAC rate/channels from the pipeline caps so
    // on_media_configure builds matching caps + codec_data (ASC).  Mirrors the
    // codec/mode detection above; the "rate=(int)" / "channels=(int)" tokens
    // appear only in the audio caps.  Defaults (44100/1) stand if absent
    // (e.g. VIDEO_ONLY).
    int detected_rate     = 44100;
    int detected_channels = 1;
    {
        const char *r = strstr(pipeline_str, "rate=(int)");
        if (r) detected_rate = atoi(r + 10);          // strlen("rate=(int)")
        const char *c = strstr(pipeline_str, "channels=(int)");
        if (c) detected_channels = atoi(c + 14);       // strlen("channels=(int)")
    }
    LOGI("nativeGstPipelineInit: audio rate=%d channels=%d",
         detected_rate, detected_channels);

    // Detect the audio codec from the pipeline string: an "opusenc" element
    // means the audio appsrc carries raw PCM (audio/x-raw); otherwise it is AAC
    // access units.  Drives the caps build in on_media_configure and the buffer
    // duration in nativePushAudioBuffer.
    bool detected_raw_pcm = (strstr(pipeline_str, "opusenc") != NULL);
    LOGI("nativeGstPipelineInit: audio codec=%s",
         detected_raw_pcm ? "Opus (raw PCM appsrc)" : "AAC");

    LOGI("nativeGstPipelineInit: acquiring lock");
    pthread_mutex_lock(&s_lock);
    LOGI("nativeGstPipelineInit: lock acquired");

    // Reject double-init.
    if (s_server) {
        LOGW("nativeGstPipelineInit: RTSP server already running – call destroy first");
        env->ReleaseStringUTFChars(jpipeline, pipeline_str);
        pthread_mutex_unlock(&s_lock);
        return JNI_FALSE;
    }

    // -------------------------------------------------------------------------
    // GMainContext + GMainLoop
    // -------------------------------------------------------------------------
    GMainContext *context = g_main_context_new();
    if (!context) {
        LOGE("nativeGstPipelineInit: g_main_context_new failed");
        env->ReleaseStringUTFChars(jpipeline, pipeline_str);
        pthread_mutex_unlock(&s_lock);
        return JNI_FALSE;
    }

    GMainLoop *loop = g_main_loop_new(context, FALSE);
    if (!loop) {
        LOGE("nativeGstPipelineInit: g_main_loop_new failed");
        g_main_context_unref(context);
        env->ReleaseStringUTFChars(jpipeline, pipeline_str);
        pthread_mutex_unlock(&s_lock);
        return JNI_FALSE;
    }

    // -------------------------------------------------------------------------
    // RTSP server
    // -------------------------------------------------------------------------
    GstRTSPServer *server = gst_rtsp_server_new();
    if (!server) {
        LOGE("nativeGstPipelineInit: gst_rtsp_server_new failed");
        g_main_loop_unref(loop);
        g_main_context_unref(context);
        env->ReleaseStringUTFChars(jpipeline, pipeline_str);
        pthread_mutex_unlock(&s_lock);
        return JNI_FALSE;
    }
    LOGI("nativeGstPipelineInit: RTSP server created: %p", server);

    // Request an IDR keyframe for every connecting client.  on_media_configure
    // fires only once (first pipeline creation), so subsequent clients rely on
    // this signal to get a clean VPS/SPS/PPS + IDR at stream start.
    g_signal_connect(server, "client-connected", G_CALLBACK(on_client_connected), NULL);

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", (int)jport);
    gst_rtsp_server_set_service(server, port_str);
    LOGI("nativeGstPipelineInit: server port set to %s", port_str);

    // -------------------------------------------------------------------------
    // Media factory – wraps the appsrc → parse → pay0 pipeline
    //
    // The factory is told (via set_media_gtype below) to construct our
    // CresRTSPMedia subclass, whose overridden setup_sdp vfunc injects the
    // a=rtcp-fb:96 nack/pli/fir attributes into the AVPF video m= section of
    // every generated SDP.
    // -------------------------------------------------------------------------
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();
    if (!factory) {
        LOGE("nativeGstPipelineInit: gst_rtsp_media_factory_new failed");
        gst_object_unref(server);
        g_main_loop_unref(loop);
        g_main_context_unref(context);
        env->ReleaseStringUTFChars(jpipeline, pipeline_str);
        pthread_mutex_unlock(&s_lock);
        return JNI_FALSE;
    }

    gst_rtsp_media_factory_set_launch(factory, pipeline_str);
    env->ReleaseStringUTFChars(jpipeline, pipeline_str);
    pipeline_str = nullptr;

    if (s_enable_rtcp_feedback) {
        // Instantiate our GstRTSPMedia subclass for every media this factory
        // constructs, so its overridden setup_sdp vfunc can inject the
        // a=rtcp-fb (nack/pli/fir) attributes into the AVPF video m= section.
        gst_rtsp_media_factory_set_media_gtype(factory, CRES_TYPE_RTSP_MEDIA);
    }

    // EOS-on-shutdown MUST be disabled for our always-on push-mode appsrc.
    //
    // With eos_shutdown=TRUE, when the last client disconnects GstRTSPMedia
    // sends EOS down the pipeline before unpreparing.  That EOS latches our
    // single shared appsrc into the EOS state.  Once an appsrc has seen EOS it
    // permanently returns GST_FLOW_EOS / GST_FLOW_FLUSHING (-3 / -2) for every
    // subsequent gst_app_src_push_buffer() call.  Historically (when the media
    // was reusable=TRUE) on_media_configure did NOT fire again for the next
    // client, so s_appsrc was never refreshed — the reconnecting client got no
    // video (observed on TCP, where the teardown path also double-unprepares
    // the media: prepare_count goes 0 → -1, "media was not prepared").
    //
    // The media is now reusable=FALSE (set per-media in on_media_configure), so
    // a fresh media+appsrc is built on every reconnect and the latched-EOS
    // failure above can no longer strand s_appsrc.  eos_shutdown=FALSE is kept
    // anyway as defense-in-depth: the encoder feeds the appsrc continuously and
    // independently of any client, so there is no benefit to pushing EOS through
    // it on disconnect, and avoiding the EOS keeps teardown clean on BOTH UDP
    // and TCP transports.
    gst_rtsp_media_factory_set_eos_shutdown(factory, FALSE);

    // Share a single pipeline instance across all RTSP clients.
    gst_rtsp_media_factory_set_shared(factory, TRUE);

    // Restrict the server to the requested lower-transport protocol so the
    // RTSP SETUP exchange only succeeds for that transport.  This prevents
    // the client silently falling back to a different transport.
    //   TCP (default): GST_RTSP_LOWER_TRANS_TCP – RTP interleaved in RTSP.
    //   UDP:           GST_RTSP_LOWER_TRANS_UDP  – separate UDP RTP sockets.
    {
#ifdef USE_RTSP_LOWER_TRANS_TCP_ONLY
        GstRTSPLowerTrans proto = (jtransport == TRANSPORT_UDP)
                ? GST_RTSP_LOWER_TRANS_UDP
                : GST_RTSP_LOWER_TRANS_TCP;

        gst_rtsp_media_factory_set_protocols(factory, proto);
        LOGI("nativeGstPipelineInit: lower-transport set to %s",
              proto == GST_RTSP_LOWER_TRANS_UDP ? "UDP" : "TCP");
#else
        // Allow BOTH UDP and TCP lower-transports so the client may pick either.
        // The bitwise OR of two enum constants is an int in C++, so cast it back
        // to the GstRTSPLowerTrans (it is a flags enum) to satisfy the type.
        GstRTSPLowerTrans proto = (GstRTSPLowerTrans)
                (GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_TCP);

        gst_rtsp_media_factory_set_protocols(factory, proto);
        LOGI("nativeGstPipelineInit: lower-transport set to UDP|TCP (client may choose)");
#endif
    }

    if (s_enable_rtcp_feedback) {
        // Advertise EXACTLY ONE profile: RTP/AVPF.
        //
        // Per RFC 4585 the a=rtcp-fb attributes (nack/pli/fir) require the
        // feedback profile, so AVPF must be enabled for them to be honoured.
        // We must NOT also enable AVP here: GStreamer emits one m=video section
        // per allowed profile, so AVP|AVPF produces TWO m=video lines in the SDP
        // (one AVP, one AVPF).  Clients such as ffmpeg/libav then create two
        // AVStreams for a single network flow and stall ("Could not find codec
        // parameters for stream 0 ... unspecified size") because they cannot
        // tell which m= line the incoming RTP belongs to.  Restricting the
        // factory to AVPF only yields a single m=video RTP/AVPF section carrying
        // the rtcp-fb attributes.
        gst_rtsp_media_factory_set_profiles(factory, GST_RTSP_PROFILE_AVPF);
        LOGI("nativeGstPipelineInit: RTP profile set to AVPF only (single m=video with rtcp-fb)");
    } else {
        // Leave the factory at its default profile (RTP/AVP only) → a single
        // plain m=video section with no rtcp-fb attributes.
        LOGI("nativeGstPipelineInit: RTCP feedback disabled – RTP/AVP only (single m=video)");
    }

    // Use PAUSE suspend mode.  The GstRTSPMediaFactory default is
    // GST_RTSP_SUSPEND_MODE_NONE, which we override here.
    //
    // With GST_RTSP_SUSPEND_MODE_NONE, two SETUP requests in rapid succession
    // (VLC sets up the video and audio streams back-to-back on this shared
    // factory) race against suspend:
    //   1. The first SETUP prepares the media; by the time the second SETUP
    //      runs, the media status has already advanced past PREPARED.
    //   2. The second SETUP calls gst_rtsp_media_suspend(), whose
    //      "status != PREPARED && status != SUSPENDED" check now fails →
    //      "was not prepared" warning → suspend returns FALSE.
    //   3. That failed suspend leaves blocking pad probes dangling on the
    //      vidPostQ / audPostQ pads.  SPS/PPS already queued slip through, but
    //      the IDR and every subsequent frame stay blocked permanently.
    //
    // GST_RTSP_SUSPEND_MODE_PAUSE instead drives the media to PAUSED on
    // suspend, so the status check always passes (PREPARED/SUSPENDED), no probe
    // is left dangling, and frames flow on PLAY.
    gst_rtsp_media_factory_set_suspend_mode(factory, GST_RTSP_SUSPEND_MODE_PAUSE);

    // Set factory latency to 0 ms for lowest-latency streaming; the sender
    // adds no buffering and relies on accurate per-buffer PTS instead.
    gst_rtsp_media_factory_set_latency(factory, 0);

    // on_media_configure fires when the RTSP server first creates the media
    // pipeline.  It stores s_appsrc so nativePushEncodedBuffer() can feed frames.
    g_signal_connect(factory, "media-configure",
                     G_CALLBACK(on_media_configure), NULL);
    LOGI("nativeGstPipelineInit: media factory configured");

    // -------------------------------------------------------------------------
    // Mount the factory at /camera.sdp
    // -------------------------------------------------------------------------
    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);
    if (!mounts) {
        LOGE("nativeGstPipelineInit: gst_rtsp_server_get_mount_points failed");
        g_object_unref(factory);
        gst_object_unref(server);
        g_main_loop_unref(loop);
        g_main_context_unref(context);
        pthread_mutex_unlock(&s_lock);
        return JNI_FALSE;
    }
    // add_factory stores the raw pointer in its GHashTable (value_destroy = g_object_unref).
    // It does NOT add a reference, so DO NOT unref factory here – the table owns the
    // only reference and will call g_object_unref when the mount point is removed.
    gst_rtsp_mount_points_add_factory(mounts, "/camera.sdp", factory);
    g_object_unref(mounts);  // server holds its own ref; release ours
    LOGI("nativeGstPipelineInit: factory mounted at /camera.sdp");

    // -------------------------------------------------------------------------
    // Attach the RTSP server to the GMainContext so it can accept connections.
    // Note: GstRTSPServer sets SO_REUSEADDR on its listening socket internally
    // via GSocketListener, so no manual socket setup is required.
    // -------------------------------------------------------------------------
    GError  *error         = NULL;
    GSource *server_source = gst_rtsp_server_create_source(server, NULL, &error);
    if (!server_source) {
        LOGE("nativeGstPipelineInit: gst_rtsp_server_create_source failed: %s",
             error ? error->message : "(unknown)");
        if (error) g_error_free(error);
        gst_object_unref(server);
        g_main_loop_unref(loop);
        g_main_context_unref(context);
        pthread_mutex_unlock(&s_lock);
        return JNI_FALSE;
    }
    g_source_attach(server_source, context);
    g_source_unref(server_source);  // context now owns the ref
    LOGI("nativeGstPipelineInit: server source attached to GMainContext");

    // -------------------------------------------------------------------------
    // Attach periodic pipeline dump timer (only when enabled).
    // Uses dump_audio_video_pipeline() which provides comprehensive element
    // hierarchy, pad/caps, properties, and optional DOT file generation.
    // Fires via the same GMainContext as the server, so it runs on the
    // GMainLoop thread with no extra locking needed.
    // -------------------------------------------------------------------------
    if (s_periodic_dump_enable) {
        GSource *periodic_source = g_timeout_source_new_seconds(s_pipeline_dump_interval_seconds);
        g_source_set_callback(periodic_source, dump_audio_video_pipeline, NULL, NULL);
        g_source_attach(periodic_source, context);
        // s_periodic_source keeps a ref so we can destroy it on pipeline teardown.
        s_periodic_source = periodic_source;  // do NOT g_source_unref here
        LOGI("nativeGstPipelineInit: %u-second pipeline dump timer attached "
             "(periodic_dump_enable=true, dot_dump=%s)",
             s_pipeline_dump_interval_seconds,
             s_pipeline_dot_dump_enable ? "true" : "false");
    } else {
        s_periodic_source = NULL;
        LOGI("nativeGstPipelineInit: pipeline dump timer skipped (periodic_dump_enable=false)");
    }

    // -------------------------------------------------------------------------
    // Start GMainLoop thread
    // -------------------------------------------------------------------------
    if (pthread_create(&s_loop_tid, NULL, loop_thread_fn, loop) != 0) {
        LOGE("nativeGstPipelineInit: pthread_create failed");
        gst_object_unref(server);
        g_main_loop_unref(loop);
        g_main_context_unref(context);
        pthread_mutex_unlock(&s_lock);
        return JNI_FALSE;
    }
    s_loop_tid_valid = true;

    // Commit state.
    s_server         = server;
    s_loop           = loop;
    s_context        = context;
    s_codec_type        = detected_codec;
    s_stream_mode       = detected_mode;
    s_audio_sample_rate = detected_rate;
    s_audio_channels    = detected_channels;
    s_audio_raw_pcm     = detected_raw_pcm;

#ifdef USE_RTSP_LOWER_TRANS_TCP_ONLY
    s_transport_mode = (TransportMode)(int)jtransport;
#endif

    s_pts_base             = GST_CLOCK_TIME_NONE;  // reset PTS base for new session
    s_last_video_pts_ns    = GST_CLOCK_TIME_NONE;  // reset A/V offset tracker
    s_av_offset_ema        = GST_CLOCK_TIME_NONE;  // reset EMA correction for new session
    s_video_push_count     = 0;
    s_audio_push_count     = 0;
    s_need_data       = true;             // allow pushes from first appsrc-ready frame
    s_audio_need_data = true;
    s_media_prepared  = false;            // push gate stays closed until on_media_prepared
    s_client_count    = 0;                // no clients connected yet
    s_encoder_pinned  = false;            // re-pin encoder thread on next push
    s_pipeline_dot_periodic_dump_count = 0; // reset DOT file counter for new session

    pthread_mutex_unlock(&s_lock);
    LOGI("nativeGstPipelineInit: RTSP server ready on port %s"
         " - connect at rtsp://<device-ip>:%s/camera.sdp", port_str, port_str);
    
    //6-08-2026: need to set this flag back to streamout app.
    char status_msg[128];
    snprintf(status_msg, sizeof(status_msg), "Stream is ready on rtsp://<device-ip>:%s/camera.sdp", port_str);
    streamout_status_fb(0, stream_status_ready, status_msg);
        
    return JNI_TRUE;
}

// ---------------------------------------------------------------------------
// nativePushEncodedBuffer
//   Java signature:
//     static native void nativePushEncodedBuffer(
//         ByteBuffer buf, int offset, int size,
//         long ptsUs, boolean isKeyFrame, boolean isCsd)
//
//   Copies the encoded AU from the MediaCodec output ByteBuffer into a new
//   GstBuffer and pushes it into the appsrc.
//
//   Ownership of gst_buf is transferred to appsrc by gst_app_src_push_buffer()
//   on both success and failure; never call gst_buffer_unref() after this.
// ---------------------------------------------------------------------------
extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativePushEncodedBuffer(
        JNIEnv    *env,
        jclass   /*clazz*/,
        jobject   jbuf,
        jint      offset,
        jint      size,
        jlong     pts_us,
        jboolean  is_key_frame,
        jboolean  is_csd)
{
    pthread_mutex_lock(&s_lock);

    if (!s_appsrc) {
        // Pipeline not yet initialised or already destroyed – silently drop.
        // NOTE: on_media_unprepared() NULLs s_appsrc the instant the media is
        // torn down, so this single check already gates out pushes while the
        // media is gone.  We do NOT additionally gate on a "prepared" flag,
        // because the GstRTSPMedia "prepared" signal does not reliably reach
        // our handler in this server build (no on_media_prepared log fires),
        // which would leave the gate closed forever and drop every frame.
        pthread_mutex_unlock(&s_lock);
        return;
    }

    // Pin the encoder callback thread to the same CPU core as the GMainLoop
    // thread on the first invocation.  MediaCodec's async output callback
    // thread is created by the framework; we pin it here because this is the
    // first point at which we run on that thread with a valid s_perf_cpu.
    // Subsequent calls are free: the flag check is a single branch-predicted
    // load under s_lock.
    if (s_cpu_affinity_enable && !s_encoder_pinned && s_perf_cpu >= 0) {
        pin_current_thread_to_cpu(s_perf_cpu);
        s_encoder_pinned = true;
        LOGI("nativePushEncodedBuffer: encoder callback thread pinned to CPU %d",
             s_perf_cpu);
    }

    // Drop P/B frames when appsrc signals its buffer is full.
    // CSD (VPS/SPS/PPS) and key frames always bypass flow control: they are
    // tiny and critical for stream recovery.  Dropping them when a client
    // reconnects causes h265parse to see an IDR without parameter sets and
    // log "broken/invalid nal SLICE_IDR", discarding the first clean frame.
    //
    // IMPORTANT: when a P/B frame is dropped, all subsequent P-frames in the
    // same GOP reference the dropped frame as their anchor and will decode as
    // corrupted / pixelated output at the receiver until the next IDR.  To
    // limit this window to a single inter-frame period (~33 ms at 30 fps),
    // set s_request_keyframe so Java's handleEncoderOutput() immediately
    // re-injects CSD and requests a new IDR from the encoder via
    // setParameters(REQUEST_SYNC_FRAME).  The mWaitingForIDR gate in Java
    // then withholds the remaining P-frames of the corrupt GOP until the
    // fresh IDR arrives.
    if (!s_need_data && !is_csd && !is_key_frame) {
        LOGD("nativePushEncodedBuffer: flow-control drop – P/B frame size=%d pts=%lldus"
             " – requesting IDR for fast decoder resync",
             (int)size, (long long)pts_us);
        s_request_keyframe = true;  // trigger CSD re-injection + IDR request in Java
        pthread_mutex_unlock(&s_lock);
        return;
    }

    // Log frame type at appropriate verbosity:
    //   CSD   → INFO  (rare, critical for stream negotiation)
    //   IDR   → DEBUG (once per keyframe interval, ~1/s)
    //   P/B   → VERBOSE (every frame, ~30/s – filtered out in normal builds)
    /*
    if (is_csd) {
        LOGI("nativePushEncodedBuffer: CSD (VPS/SPS/PPS) size=%d pts=%lldus",
             (int)size, (long long)pts_us);
    } else if (is_key_frame) {
        LOGD("nativePushEncodedBuffer: IDR/key frame size=%d pts=%lldus",
             (int)size, (long long)pts_us);
    } else {
        LOGV("nativePushEncodedBuffer: P/B frame size=%d pts=%lldus",
             (int)size, (long long)pts_us);
    }
    */

    // ---------------------------------------------------------------------------
    // Obtain a pointer to the encoded data.
    // MediaCodec output ByteBuffers are always direct on Android, so
    // GetDirectBufferAddress succeeds without extra allocation.
    // ---------------------------------------------------------------------------
    const uint8_t *src = nullptr;
    jbyteArray     jarray     = nullptr;
    jbyte         *jarray_ptr = nullptr;

    src = static_cast<const uint8_t *>(env->GetDirectBufferAddress(jbuf));
    if (src) {
        src += offset;
    } else {
        // Fallback for non-direct ByteBuffer (should not normally happen with
        // MediaCodec, but handle defensively).
        LOGW("nativePushEncodedBuffer: non-direct ByteBuffer – using byte array");
        jclass    bbClass = env->GetObjectClass(jbuf);
        jmethodID toArr   = env->GetMethodID(bbClass, "array", "()[B");
        if (!toArr) {
            LOGE("nativePushEncodedBuffer: could not resolve ByteBuffer.array()");
            pthread_mutex_unlock(&s_lock);
            return;
        }
        jarray      = static_cast<jbyteArray>(env->CallObjectMethod(jbuf, toArr));
        jarray_ptr  = env->GetByteArrayElements(jarray, nullptr);
        src         = reinterpret_cast<const uint8_t *>(jarray_ptr) + offset;
    }

    // ---------------------------------------------------------------------------
    // Allocate a GstBuffer and copy the encoded AU.
    // This is the single memcpy in the pipeline; encoded video frames are small
    // (a few KB for P-frames, tens of KB for I-frames at typical bitrates).
    // ---------------------------------------------------------------------------
    GstBuffer *gst_buf = gst_buffer_new_allocate(NULL, static_cast<gsize>(size), NULL);
    GstMapInfo map;
    if (!gst_buffer_map(gst_buf, &map, GST_MAP_WRITE)) {
        LOGE("nativePushEncodedBuffer: gst_buffer_map failed");
        gst_buffer_unref(gst_buf);
        if (jarray_ptr) env->ReleaseByteArrayElements(jarray, jarray_ptr, JNI_ABORT);
        pthread_mutex_unlock(&s_lock);
        return;
    }
    memcpy(map.data, src, static_cast<size_t>(size));
    gst_buffer_unmap(gst_buf, &map);

    if (jarray_ptr) {
        env->ReleaseByteArrayElements(jarray, jarray_ptr, JNI_ABORT);
    }

    // Assign hardware-accurate PTS from the Camera2 sensor timestamp.
    // MediaCodec presentationTimeUs == Camera2 sensor timestamp in µs (CLOCK_MONOTONIC).
    // These are generated by the ISP hardware with <100 µs jitter, giving the
    // RTP payloader precisely-spaced timestamps.  The receiver's jitter buffer
    // can therefore be configured much smaller (<20 ms vs the typical 200 ms
    // default), directly reducing glass-to-glass latency.
    //
    // Normalise to t=0 at the first non-CSD frame so GStreamer does not see
    // large (~seconds-since-boot) initial PTS values that confuse some parsers.
    // CSD buffers (SPS/PPS) intentionally receive no PTS; do-timestamp=TRUE on
    // the appsrc provides a clock-based fallback for those.
    if (!is_csd && pts_us > 0) {
        GstClockTime pts_ns = (GstClockTime)pts_us * GST_USECOND;
        if (s_pts_base == GST_CLOCK_TIME_NONE) {
            s_pts_base = pts_ns;
            LOGI("nativePushEncodedBuffer: PTS base set to %llu ns (%.3f s)",
                 (unsigned long long)pts_ns, pts_ns / 1e9);
        }
        GstClockTime norm_pts = (pts_ns >= s_pts_base) ? (pts_ns - s_pts_base) : 0;
        GST_BUFFER_PTS(gst_buf) = norm_pts;

        // Track for A/V offset computation in nativePushAudioBuffer.
        s_last_video_pts_ns = norm_pts;

        // Periodic log every ~3 s (90 frames at 30 fps).
        if ((++s_video_push_count % 90) == 0) {
            LOGD("nativePushEncodedBuffer [video PTS]: %.3f s  %s",
                 norm_pts / 1e9, is_key_frame ? "IDR" : "P");
        }
    }

    // Mark non-keyframes so downstream elements can drop them if needed.
    if (!is_key_frame && !is_csd) {
        GST_BUFFER_FLAG_SET(gst_buf, GST_BUFFER_FLAG_DELTA_UNIT);
    }

    // ---------------------------------------------------------------------------
    // Push.  gst_app_src_push_buffer() ALWAYS takes ownership of gst_buf,
    // regardless of the return value.  Never call gst_buffer_unref() after this.
    // ---------------------------------------------------------------------------
    GstFlowReturn flow =
            gst_app_src_push_buffer(GST_APP_SRC(s_appsrc), gst_buf);

    if (flow == GST_FLOW_OK) {
        //LOGV("nativePushEncodedBuffer: push OK – %s size=%d pts=%lldus",
        //     is_csd ? "CSD" : (is_key_frame ? "IDR" : "P/B"),
        //     (int)size, (long long)pts_us);
    } else {
        // gst_app_src_push_buffer() return values (GstFlowReturn):
        //   GST_FLOW_OK       =  0
        //   GST_FLOW_NOT_LINKED = -1
        //   GST_FLOW_FLUSHING = -2  (pad/element flushing or not PLAYING)
        //   GST_FLOW_EOS      = -3  (appsrc already saw EOS – will never recover)
        //   GST_FLOW_ERROR    = -5
        // -2/-3 after a client disconnect indicate the appsrc was latched by an
        // EOS/teardown (see eos_shutdown=FALSE above).  Print the symbolic name
        // so the value is not mistaken for GST_FLOW_ERROR.
        const char *flow_name =
            (flow == GST_FLOW_NOT_LINKED) ? "GST_FLOW_NOT_LINKED" :
            (flow == GST_FLOW_FLUSHING)   ? "GST_FLOW_FLUSHING"   :
            (flow == GST_FLOW_EOS)        ? "GST_FLOW_EOS"        :
            (flow == GST_FLOW_ERROR)      ? "GST_FLOW_ERROR"      : "GST_FLOW_<other>";
        LOGE("nativePushEncodedBuffer: push FAILED flow=%d (%s) - %s size=%d pts=%lldus",
             static_cast<int>(flow), flow_name,
             is_csd ? "CSD" : (is_key_frame ? "IDR" : "P/B"),
             (int)size, (long long)pts_us);
    }

    pthread_mutex_unlock(&s_lock);
}

// ---------------------------------------------------------------------------
// nativePushAudioBuffer
//   Java signature:
//     static native void nativePushAudioBuffer(
//         ByteBuffer buf, int offset, int size, long ptsUs)
//
//   Copies one raw AAC-LC frame from the MediaCodec audio encoder output into
//   a GstBuffer and pushes it into the audio appsrc.
//
//   PTS handling: once the first video frame has established s_pts_base, this
//   function SETS GST_BUFFER_PTS = corrected_pts (raw audio PTS normalised to
//   s_pts_base and shifted back by the EMA A/V-sync offset) plus an explicit
//   GST_BUFFER_DURATION (one AAC-LC frame).  do-timestamp=TRUE on the audio
//   appsrc is only a FALLBACK: per the appsrc contract it stamps running_time
//   ONLY when both DTS and PTS are GST_CLOCK_TIME_NONE, which here happens just
//   for the early frames pushed before the first video frame arrives.
// ---------------------------------------------------------------------------
extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativePushAudioBuffer(
        JNIEnv  *env,
        jclass  /*clazz*/,
        jobject  jbuf,
        jint     offset,
        jint     size,
        jlong    pts_us)
{
    pthread_mutex_lock(&s_lock);

    if (!s_audio_appsrc) {
        pthread_mutex_unlock(&s_lock);
        return;
    }

    // Simple flow-control: drop audio when appsrc is full.
    // Audio frames are tiny (~200 B) and CBR; a single dropped frame produces
    // at most one ~23 ms gap, which is acceptable.
    if (!s_audio_need_data) {
        pthread_mutex_unlock(&s_lock);
        return;
    }

    const uint8_t *src = nullptr;
    jbyteArray     jarray     = nullptr;
    jbyte         *jarray_ptr = nullptr;

    src = static_cast<const uint8_t *>(env->GetDirectBufferAddress(jbuf));
    if (src) {
        src += offset;
    } else {
        jclass    bbClass = env->GetObjectClass(jbuf);
        jmethodID toArr   = env->GetMethodID(bbClass, "array", "()[B");
        if (!toArr) {
            LOGE("nativePushAudioBuffer: could not resolve ByteBuffer.array()");
            pthread_mutex_unlock(&s_lock);
            return;
        }
        jarray      = static_cast<jbyteArray>(env->CallObjectMethod(jbuf, toArr));
        jarray_ptr  = env->GetByteArrayElements(jarray, nullptr);
        src         = reinterpret_cast<const uint8_t *>(jarray_ptr) + offset;
    }

    GstBuffer *gst_buf = gst_buffer_new_allocate(NULL, static_cast<gsize>(size), NULL);
    GstMapInfo map;
    if (!gst_buffer_map(gst_buf, &map, GST_MAP_WRITE)) {
        LOGE("nativePushAudioBuffer: gst_buffer_map failed");
        gst_buffer_unref(gst_buf);
        if (jarray_ptr) env->ReleaseByteArrayElements(jarray, jarray_ptr, JNI_ABORT);
        pthread_mutex_unlock(&s_lock);
        return;
    }
    memcpy(map.data, src, static_cast<size_t>(size));
    gst_buffer_unmap(gst_buf, &map);

    if (jarray_ptr) {
        env->ReleaseByteArrayElements(jarray, jarray_ptr, JNI_ABORT);
    }

    // ---------------------------------------------------------------------------
    // Audio PTS – EMA-corrected to match the video timeline.
    //
    // A/V OFFSET — measured reality (re-verified in logcat.txt on TSx,
    // 2026-06-17, full ~65 s session).  This SUPERSEDES an earlier run that
    // reported a negative / audio-behind offset.
    //
    // Clock check (METHOD): SENSOR_INFO_TIMESTAMP_SOURCE, read in listCameras(),
    // returned 1 = REALTIME = CLOCK_BOOTTIME.  The audio PTS uses
    // elapsedRealtimeNanos() = CLOCK_BOOTTIME too.  So audio and video are on the
    // SAME clock; both raw_audio_pts and last_video_pts are honest capture-times
    // and the shared s_pts_base cancels in their difference:
    //
    //   raw_audio_pts  = audio_capture_start − s_pts_base
    //   last_video_pts = camera_capture_ts   − s_pts_base
    //   instant_offset = raw_audio_pts − last_video_pts
    //                  = audio_capture_start − camera_capture_ts
    //
    // OBSERVED: instant_offset is POSITIVE and stable, ≈ +66..+110 ms
    // (mean ≈ +85 ms) — the audio PTS runs AHEAD of the video PTS, every frame
    // for the whole session.  Example line from logcat.txt:
    //   raw_audio=2.367 s  video=2.267 s  ema_offset=91 ms  corrected=2.276 s
    //   A/V_offset=8 ms
    // This CONFIRMS the original "+85 ms audio-AHEAD" observation; the later
    // "negative / audio-behind" narrative was a one-off measurement and does NOT
    // hold for this build.
    //
    // CURRENT BEHAVIOUR of the EMA below — it IS doing real work:
    //   • instant_offset is positive every frame, so the [0,500 ms] clamp does
    //     NOT zero it; s_av_offset_ema converges to and tracks ≈ +85 ms
    //     (observed 71..98 ms).
    //   • corrected_pts = raw_audio_pts − s_av_offset_ema shifts the audio PTS
    //     back by ~85 ms so it lands on the video timeline.
    //   • The post-correction residual (logged as A/V_offset = corrected − video)
    //     then oscillates around 0, ≈ ±20 ms, mostly within the ≤10 ms target.
    //     That residual dips slightly negative on some frames — do NOT mistake it
    //     for a negative *instant* offset; it is the corrected leftover, not the
    //     raw (raw_audio − video) difference.
    //
    // So WITHOUT this EMA correction the audio would arrive ~85 ms ahead of video
    // at the client.  Removing/altering the correction needs new measurement.
    // ---------------------------------------------------------------------------
    if (pts_us > 0 && s_pts_base != GST_CLOCK_TIME_NONE) {
        GstClockTime pts_ns       = (GstClockTime)pts_us * GST_USECOND;
        GstClockTime raw_audio_pts = (pts_ns >= s_pts_base) ? (pts_ns - s_pts_base) : 0;

        // Update EMA with the instantaneous A/V offset.
        if (s_last_video_pts_ns != GST_CLOCK_TIME_NONE) {
            gint64 instant_offset_ns = (gint64)raw_audio_pts - (gint64)s_last_video_pts_ns;
            // Clamp to [0, 500 ms].  NOTE (measured, logcat.txt 2026-06-17):
            // instant_offset is POSITIVE essentially every frame (≈ +66..+110 ms,
            // audio PTS ahead of video), so this clamp does NOT fire in steady
            // state — s_av_offset_ema tracks the real ~+85 ms offset and the
            // correction below is active (shifts audio back onto the video
            // timeline).  The lower clamp at 0 only guards the rare jittered
            // frame whose raw offset computes slightly negative.
            if (instant_offset_ns < 0) instant_offset_ns = 0;
            if (instant_offset_ns > (gint64)500000000LL) instant_offset_ns = 500000000LL;

            if (s_av_offset_ema == GST_CLOCK_TIME_NONE) {
                // Cold start: initialise immediately to the first observation.
                s_av_offset_ema = (GstClockTime)instant_offset_ns;
                if (s_audio_latency_debug)
                    LOGI("nativePushAudioBuffer [EMA init]: offset=%" G_GINT64_FORMAT " ms"
                         "  (measured raw offset is POSITIVE ≈ +85 ms — audio PTS"
                         " ahead of video; EMA tracks it; see block comment above)",
                         instant_offset_ns / 1000000LL);
            } else {
                s_av_offset_ema = (GstClockTime)(
                    AV_OFFSET_ALPHA * (double)instant_offset_ns
                    + (1.0 - AV_OFFSET_ALPHA) * (double)s_av_offset_ema);
            }
        }

        // Apply EMA correction: shift audio PTS back by the smoothed offset so
        // audio aligns with the video timeline (both anchored to camera capture).
        GstClockTime corrected_pts = 0;
        if (s_av_offset_ema != GST_CLOCK_TIME_NONE && raw_audio_pts > s_av_offset_ema) {
            corrected_pts = raw_audio_pts - s_av_offset_ema;
        }

        GST_BUFFER_PTS(gst_buf)      = corrected_pts;
        // Frame duration depends on what this buffer carries:
        //   • Opus path (s_audio_raw_pcm): raw PCM, so duration = nsamples / rate
        //     where nsamples = size / (2 bytes * channels).  e.g. a 10 ms @ 48000
        //     mono chunk (960 B) → 480 samples → 10 ms.
        //   • AAC path: one AAC-LC access unit = 1024 samples, so
        //     duration = 1024 / rate (≈23.2 ms @ 44100, ≈21.3 ms @ 48000).
        // Correct duration lets the parser/payloader interpolate RTP timestamps.
        if (s_audio_raw_pcm) {
            int ch = (s_audio_channels > 0) ? s_audio_channels : 1;
            guint64 nsamples = (guint64)size / (guint64)(2 * ch);
            GST_BUFFER_DURATION(gst_buf) =
                nsamples * GST_SECOND / (guint64)s_audio_sample_rate;
        } else {
            GST_BUFFER_DURATION(gst_buf) =
                1024ULL * GST_SECOND / (guint64)s_audio_sample_rate;
        }

        // Periodic diagnostic log (every 50 audio frames ≈ every 1.2 s).
        // Look for "A/V_offset" in logcat.  MEASURED on TSx (logcat.txt
        // 2026-06-17): the RAW offset (raw_audio − video) is POSITIVE ≈ +85 ms
        // (audio PTS ahead of video); ema_offset tracks it (~71..98 ms) and the
        // correction is applied, so the A/V_offset logged here is the POST-
        // correction residual, which oscillates ≈ ±20 ms around 0 (near the
        // ≤10 ms target).  Both streams are CLOCK_BOOTTIME — no clock mismatch.
        if ((++s_audio_push_count % 50) == 0 && s_audio_latency_debug) {
            gint64 residual_ms = 0;
            if (s_last_video_pts_ns != GST_CLOCK_TIME_NONE)
                residual_ms = ((gint64)corrected_pts - (gint64)s_last_video_pts_ns) / 1000000LL;
            LOGI("nativePushAudioBuffer [A/V sync]: "
                 "raw_audio=%.3f s  video=%.3f s  ema_offset=%lld ms  "
                 "corrected=%.3f s  A/V_offset=%lld ms"
                 "  (target ≤ 10 ms; >50 ms = client adds buffer)",
                 raw_audio_pts / 1e9,
                 s_last_video_pts_ns != GST_CLOCK_TIME_NONE
                         ? s_last_video_pts_ns / 1e9 : -0.0,
                 (long long)(s_av_offset_ema != GST_CLOCK_TIME_NONE
                         ? s_av_offset_ema / 1000000LL : 0LL),
                 corrected_pts / 1e9,
                 (long long)residual_ms);
        }
    }
    // else: s_pts_base not yet set (first video frame not yet received).
    // GST_BUFFER_PTS remains GST_CLOCK_TIME_NONE; do-timestamp=TRUE on the
    // audio appsrc provides a running_time fallback for these early frames.

    GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(s_audio_appsrc), gst_buf);
    if (flow != GST_FLOW_OK) {
        LOGE("nativePushAudioBuffer: push FAILED flow=%d size=%d pts=%lldus",
             static_cast<int>(flow), (int)size, (long long)pts_us);
    }

    pthread_mutex_unlock(&s_lock);
}

// ---------------------------------------------------------------------------
// nativeGstPipelineDestroy
//   Java signature: static native void nativeGstPipelineDestroy()
//
//   Sends EOS to appsrc, quits the GMainLoop, joins the loop thread,
//   and releases all GStreamer/GLib resources.
//   Safe to call even if nativeGstPipelineInit() was never called.
// ---------------------------------------------------------------------------
extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativeGstPipelineDestroy(
        JNIEnv * /*env*/, jclass /*clazz*/)
{
    LOGI("nativeGstPipelineDestroy: called");

    pthread_mutex_lock(&s_lock);

    // Signal end-of-stream to appsrc so connected clients receive a clean EOS.
    if (s_appsrc) {
        gst_app_src_end_of_stream(GST_APP_SRC(s_appsrc));
        gst_object_unref(s_appsrc);
        s_appsrc = NULL;
    }
    if (s_audio_appsrc) {
        gst_app_src_end_of_stream(GST_APP_SRC(s_audio_appsrc));
        gst_object_unref(s_audio_appsrc);
        s_audio_appsrc = NULL;
    }
    s_need_data = false;
    s_audio_need_data = false;
    s_media_prepared = false;
    s_client_count = 0;
    s_pts_base          = GST_CLOCK_TIME_NONE;
    s_last_video_pts_ns = GST_CLOCK_TIME_NONE;
    s_av_offset_ema     = GST_CLOCK_TIME_NONE;
    s_pipeline_dot_periodic_dump_count = 0;

    GMainLoop     *loop    = s_loop;
    GMainContext  *context = s_context;
    GstRTSPServer *server  = s_server;

    s_loop    = NULL;
    s_context = NULL;
    s_server  = NULL;

    // Release cached full pipeline reference.
    if (s_full_pipeline) {
        gst_object_unref(s_full_pipeline);
        s_full_pipeline = NULL;
    }

    // Cancel the periodic element-dump timer before quitting the loop.
    if (s_periodic_source) {
        g_source_destroy(s_periodic_source);  // removes from context
        g_source_unref(s_periodic_source);    // release our ref
        s_periodic_source = NULL;
    }

    pthread_mutex_unlock(&s_lock);

    // Quit the main loop so the loop thread exits.
    if (loop) {
        g_main_loop_quit(loop);
    }

    // Join the loop thread outside the lock to avoid deadlock.
    if (s_loop_tid_valid) {
        pthread_join(s_loop_tid, NULL);
        s_loop_tid_valid = false;
    }

    // Release GStreamer/GLib resources in reverse allocation order.
    if (server)  gst_object_unref(server);
    if (loop)    g_main_loop_unref(loop);
    if (context) g_main_context_unref(context);

    
    //6-08-2026: need to set this flag back to streamout app.
    streamout_status_fb(0, stream_status_stopped, "Stream is destroyed");

    LOGI("nativeGstPipelineDestroy: complete");
}

// ---------------------------------------------------------------------------
// nativeConsumeKeyframeRequest
//   Java signature: static native boolean nativeConsumeKeyframeRequest()
//
//   Returns true (and atomically clears the flag) if on_media_configure has
//   fired since the last call – indicating that a new RTSP client connected
//   and an IDR keyframe should be requested from the encoder immediately.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Feature-flag JNI setters
// Each can be called at any time (before or after nativeGstPipelineInit).
// ---------------------------------------------------------------------------
extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativeSetCpuAffinityEnable(
        JNIEnv * /*env*/, jclass /*clazz*/, jboolean enable)
{
    pthread_mutex_lock(&s_lock);
    s_cpu_affinity_enable = (enable == JNI_TRUE);
    if (!s_cpu_affinity_enable) {
        // Reset so the encoder thread is re-evaluated if the flag is toggled back on.
        s_encoder_pinned = false;
        s_perf_cpu = -1;
    }
    pthread_mutex_unlock(&s_lock);
    LOGI("nativeSetCpuAffinityEnable: cpu_affinity_enable=%s",
         enable ? "true" : "false");
}

extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativeSetTcpNoDelay(
        JNIEnv * /*env*/, jclass /*clazz*/, jboolean enable)
{
    pthread_mutex_lock(&s_lock);
    s_tcp_no_delay = (enable == JNI_TRUE);
    pthread_mutex_unlock(&s_lock);
    LOGI("nativeSetTcpNoDelay: tcp_no_delay=%s", enable ? "true" : "false");
}

extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativeSetSoSndbufSize(
        JNIEnv * /*env*/, jclass /*clazz*/, jint size)
{
    pthread_mutex_lock(&s_lock);
    s_so_sndbuf_size = (int)size;
    pthread_mutex_unlock(&s_lock);
    LOGI("nativeSetSoSndbufSize: so_sndbuf_size=%d", (int)size);
}

extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativeSetTcpNotsentLowat(
        JNIEnv * /*env*/, jclass /*clazz*/, jint bytes)
{
    pthread_mutex_lock(&s_lock);
    s_tcp_notsent_lowat = (int)bytes;
    pthread_mutex_unlock(&s_lock);
    LOGI("nativeSetTcpNotsentLowat: tcp_notsent_lowat=%d", (int)bytes);
}

extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativeSetIpTosDscpEf(
        JNIEnv * /*env*/, jclass /*clazz*/, jboolean enable)
{
    pthread_mutex_lock(&s_lock);
    s_ip_tos_dscp_ef = (enable == JNI_TRUE);
    pthread_mutex_unlock(&s_lock);
    LOGI("nativeSetIpTosDscpEf: ip_tos_dscp_ef=%s", enable ? "true" : "false");
}

extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativeSetH26xParseEnable(
        JNIEnv * /*env*/, jclass /*clazz*/, jboolean enable)
{
    pthread_mutex_lock(&s_lock);
    s_h26x_parse_enable = (enable == JNI_TRUE);
    pthread_mutex_unlock(&s_lock);
    LOGI("nativeSetH26xParseEnable: h26x_parse_enable=%s", enable ? "true" : "false");
}

extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativeSetRtpbinBufferModeNone(
        JNIEnv * /*env*/, jclass /*clazz*/, jboolean enable)
{
    pthread_mutex_lock(&s_lock);
    s_rtpbin_buffer_mode_none = (enable == JNI_TRUE);
    pthread_mutex_unlock(&s_lock);
    LOGI("nativeSetRtpbinBufferModeNone: rtpbin_buffer_mode_none=%s",
         enable ? "true" : "false");
}

extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativeSetPeriodicDumpEnable(
        JNIEnv * /*env*/, jclass /*clazz*/, jboolean enable)
{
    pthread_mutex_lock(&s_lock);
    s_periodic_dump_enable = (enable == JNI_TRUE);
    pthread_mutex_unlock(&s_lock);
    LOGI("nativeSetPeriodicDumpEnable: periodic_dump_enable=%s",
         enable ? "true" : "false");
}

extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativeSetRtcpFeedback(
        JNIEnv * /*env*/, jclass /*clazz*/, jboolean enable)
{
    pthread_mutex_lock(&s_lock);
    s_enable_rtcp_feedback = (enable == JNI_TRUE);
    pthread_mutex_unlock(&s_lock);
    LOGI("nativeSetRtcpFeedback: rtcp_feedback=%s", enable ? "true" : "false");
}

extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativeSetPipelineDumpInterval(
        JNIEnv * /*env*/, jclass /*clazz*/, jint seconds)
{
    pthread_mutex_lock(&s_lock);
    s_pipeline_dump_interval_seconds = (seconds > 0) ? (guint)seconds : 30;
    pthread_mutex_unlock(&s_lock);
    LOGI("nativeSetPipelineDumpInterval: pipeline_dump_interval_seconds=%u",
         s_pipeline_dump_interval_seconds);
}

extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativeSetPipelineDotDumpEnable(
        JNIEnv * /*env*/, jclass /*clazz*/, jboolean enable)
{
    pthread_mutex_lock(&s_lock);
    s_pipeline_dot_dump_enable = (enable == JNI_TRUE);
    pthread_mutex_unlock(&s_lock);
    LOGI("nativeSetPipelineDotDumpEnable: pipeline_dot_dump_enable=%s",
         enable ? "true" : "false");
}

extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativeSetPipelineDotOutputDir(
        JNIEnv *env, jclass /*clazz*/, jstring jdir)
{
    const char *dir = env->GetStringUTFChars(jdir, nullptr);
    if (!dir) {
        LOGE("nativeSetPipelineDotOutputDir: GetStringUTFChars failed");
        return;
    }
    pthread_mutex_lock(&s_lock);
    snprintf(s_pipeline_dot_output_dir, sizeof(s_pipeline_dot_output_dir), "%s", dir);
    pthread_mutex_unlock(&s_lock);
    LOGI("nativeSetPipelineDotOutputDir: output_dir=%s", s_pipeline_dot_output_dir);
    env->ReleaseStringUTFChars(jdir, dir);
}

extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativeDumpPipelineNow(
        JNIEnv * /*env*/, jclass /*clazz*/)
{
    // On-demand pipeline dump – bypasses the periodic timer and the 3-file
    // limit.  Can be called at any time from Java for ad-hoc diagnostics.
    pthread_mutex_lock(&s_lock);
    GstBin *pipeline = s_full_pipeline
        ? GST_BIN(gst_object_ref(s_full_pipeline)) : NULL;
    pthread_mutex_unlock(&s_lock);

    if (!pipeline) {
        LOGW("nativeDumpPipelineNow: no pipeline available – has a client connected?");
        return;
    }

    LOGI("nativeDumpPipelineNow: on-demand dump requested");
    dump_pipeline_to_dot_file(pipeline, "on-demand");

    // Also run the full element dump
    dump_audio_video_pipeline(NULL);

    gst_object_unref(pipeline);
}

// ---------------------------------------------------------------------------
extern "C" JNIEXPORT jboolean JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativeConsumeKeyframeRequest(
        JNIEnv * /*env*/, jclass /*clazz*/)
{
    pthread_mutex_lock(&s_lock);
    bool requested = s_request_keyframe;
    s_request_keyframe = false;
    pthread_mutex_unlock(&s_lock);
    return requested ? JNI_TRUE : JNI_FALSE;
}


// Returns a ref'd handle to the cached full pipeline (NULL until a client
// has played).  Caller MUST gst_object_unref() when done.
extern "C" GstBin *Cam2Streamer_RefFullPipeline(void)
{
    pthread_mutex_lock(&s_lock);
    GstBin *p = s_full_pipeline ? GST_BIN(gst_object_ref(s_full_pipeline)) : NULL;
    pthread_mutex_unlock(&s_lock);
    return p;
}

// Toggles the NATIVE audio-latency diagnostic logging (the "Opus encode
// latency" pad-probe print and the "[A/V sync]" / "[EMA init]" prints).  Called
// from the cam2_streamer_debug SET_AUDIO_LATENCY_DEBUG command.  LOGGING ONLY —
// the encode-probe statistics and the EMA PTS correction keep running; this
// only gates the LOGI calls.  No lock needed: a single int flag read on the
// streaming/audio threads, written here from the debug thread.
extern "C" void Cam2Streamer_SetAudioLatencyDebug(int on)
{
    int newval = on ? 1 : 0;
    // On re-enable, clear any stale sink entries left in the FIFO map from
    // before it was disabled, so the first paired window starts clean (the src
    // probe early-returned while off, so old sink arrivals were never popped).
    if (newval && !s_audio_latency_debug)
        opus_probe_reset();
    s_audio_latency_debug = newval;
    LOGI("Cam2Streamer_SetAudioLatencyDebug: native audio-latency logging = %s",
         s_audio_latency_debug ? "ON" : "OFF");
}