/**
 * Copyright (C) 2026 to the present, Crestron Electronics, Inc.
 * All rights reserved.
 *
 * \file        cam2_streamout_debug.cpp
 *
 * \brief       Debug command dispatcher for the Camera2 + MediaCodec
 *              streamOutApk pipeline (camera2_gst_streamer.cpp).
 *
 *              Mirrors the relevant subset of cresStreamOutDebug.cpp but
 *              targets the cached full pipeline (s_full_pipeline) of the
 *              Camera2 path instead of CStreamoutManager::m_pMedia.
 *
 *              Supported commands (case-insensitive, args separated by ',' or ' '):
 *                  PRINT_ELEMENT_PROPERTY  <element_name>
 *                  SET_CATEGORY_DEBUG_LEVEL <category_name> <level>
 *                  INSPECT_ELEMENT         <factory_name>
 *                  SET_MAX_BITRATE         <bps | K/M/G suffix, e.g. 20M>
 *                  SET_RTCP_FEEDBACK       <true|false>
 *                  SET_PREFER_H265         <true|false>
 *                  SET_STREAM_MODE         <0|1|2 | VIDEO_ONLY|AUDIO_ONLY|VIDEO_AND_AUDIO_BOTH>
 *                  SET_AUDIO_SAMPLE_RATE   <Hz, e.g. 44100 | 48000>
 *                  SET_AUDIO_CHANNELS      <1|2  (1=mono, 2=stereo)>
 *                  SET_USE_OPUS            <true|false  (Opus vs AAC audio)>
 *                  SET_AUDIO_LATENCY_DEBUG <true|false  (audio latency logs on/off)>
 *                  DUMP_ALL_CONFIG         (no args; logs all current settings)
 *                  DUMP_PIPELINE_GRAPH     (no args; writes a .dot graph + logs full pipeline)
 *
 *              Entry point:
 *                  void cam2_streamer_debug(char *cmd_cstring);
 *
 *              The caller is expected to pass a string whose first token is
 *              the prefix (e.g. "STREAMOUT_CAM2") and whose subsequent tokens
 *              are the command and its arguments — matching the calling
 *              convention used by jni_rtsp_server_debug().
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <android/log.h>
#include <jni.h>
#include <gst/gst.h>

#include "include/gst_element_print_properties.h"

#define LOG_TAG "strmout_DebugCam2"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Provided by camera2_gst_streamer.cpp – returns a ref'd handle to the
// cached full pipeline (NULL if no client has played yet).
extern "C" GstBin *Cam2Streamer_RefFullPipeline(void);

// Provided by camera2_gst_streamer.cpp – the JNI on-demand pipeline dump
// (writes a Graphviz .dot graph of the live pipeline AND logs the full element
// hierarchy to logcat).  Its JNIEnv*/jclass parameters are unused by the
// implementation, so it is safe to call directly with nullptr from here
// instead of going through a JNI round-trip.
extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_Camera2GstStreamer_nativeDumpPipelineNow(JNIEnv *env, jclass clazz);

// Provided by camera2_gst_streamer.cpp – toggles the NATIVE audio-latency
// diagnostic logging (the "Opus encode latency" pad-probe print and the
// "[A/V sync]" / "[EMA init]" PTS prints).  Logging only; the encode-probe
// stats and the EMA PTS correction keep running regardless.
extern "C" void Cam2Streamer_SetAudioLatencyDebug(int on);

// Provided by jniMock.cpp.
extern "C" void LocalConvertToUpper(char *str);

// Cached JavaVM (set in JNI_OnLoad, streamout.cpp); used to call back into
// Java for runtime settings such as the encoder max bitrate.
extern JavaVM *g_vm;

// Public entry point.
extern "C" void cam2_streamer_debug(char *cmd_cstring);

// ---------------------------------------------------------------------------
// Command table
// ---------------------------------------------------------------------------
enum
{
    CAM2_DBG_PRINT_ELEMENT_PROPERTY = 0,
    CAM2_DBG_SET_CATEGORY_DEBUG_LEVEL,
    CAM2_DBG_INSPECT_ELEMENT,
    CAM2_DBG_SET_MAX_BITRATE,
    CAM2_DBG_SET_RTCP_FEEDBACK,
    CAM2_DBG_SET_PREFER_H265,
    CAM2_DBG_SET_STREAM_MODE,
    CAM2_DBG_SET_AUDIO_SAMPLE_RATE,
    CAM2_DBG_SET_AUDIO_CHANNELS,
    CAM2_DBG_SET_USE_OPUS,
    CAM2_DBG_SET_AUDIO_LATENCY_DEBUG,
    CAM2_DBG_DUMP_ALL_CONFIG,
    CAM2_DBG_DUMP_PIPELINE_GRAPH,
    MAX_CAM2_DBG_NUM
};

static const char * const cam2_dbg_names[MAX_CAM2_DBG_NUM] =
{
    "PRINT_ELEMENT_PROPERTY",
    "SET_CATEGORY_DEBUG_LEVEL",
    "INSPECT_ELEMENT",
    "SET_MAX_BITRATE",
    "SET_RTCP_FEEDBACK",
    "SET_PREFER_H265",
    "SET_STREAM_MODE",
    "SET_AUDIO_SAMPLE_RATE",
    "SET_AUDIO_CHANNELS",
    "SET_USE_OPUS",
    "SET_AUDIO_LATENCY_DEBUG",
    "DUMP_ALL_CONFIG",
    "DUMP_PIPELINE_GRAPH",
};

static void cam2_streamer_debug_printHelp(void)
{
    LOGI("Camera2 stream-out debug commands:");
    for (int i = 0; i < MAX_CAM2_DBG_NUM; i++) {
        LOGI("  %s", cam2_dbg_names[i]);
    }
}

static int cam2_streamer_debug_getIndex(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < MAX_CAM2_DBG_NUM; i++) {
        if (strncmp(cam2_dbg_names[i], name, strlen(name)) == 0) {
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

// PRINT_ELEMENT_PROPERTY <element_name>
//   Looks up <element_name> inside the cached camera2 full pipeline and
//   prints all of its properties.
static void cam2_dbg_print_element_property(const char *elementName)
{
    if (!elementName) {
        LOGE("PRINT_ELEMENT_PROPERTY: missing element name");
        return;
    }

    GstBin *pipeline = Cam2Streamer_RefFullPipeline();
    if (!pipeline) {
        LOGE("PRINT_ELEMENT_PROPERTY: pipeline not ready (no client played yet)");
        return;
    }

    gchar *binName = gst_element_get_name(GST_ELEMENT(pipeline));
    LOGI("PRINT_ELEMENT_PROPERTY: searching pipeline[%s] for '%s'",
         binName ? binName : "(null)", elementName);

    GstElement *ele = gst_bin_get_by_name_recurse_up(GST_BIN(pipeline), elementName);
    if (ele) {
        gst_element_print_properties(ele);
        gst_object_unref(ele);
    } else {
        LOGE("PRINT_ELEMENT_PROPERTY: element '%s' not found", elementName);
    }

    g_free(binName);
    gst_object_unref(pipeline);
}

// SET_CATEGORY_DEBUG_LEVEL <category_name> <level>
//   Sets the GStreamer debug threshold for the given category.
static void cam2_dbg_set_category_debug_level(const char *categoryName, const char *levelStr)
{
    if (!categoryName) {
        LOGE("SET_CATEGORY_DEBUG_LEVEL: missing category name");
        return;
    }
    if (!levelStr) {
        LOGE("SET_CATEGORY_DEBUG_LEVEL: missing level value");
        return;
    }

    char *endPtr = NULL;
    int level = (int)strtol(levelStr, &endPtr, 10);
    gst_debug_set_threshold_for_name(categoryName, (GstDebugLevel)level);
    LOGI("SET_CATEGORY_DEBUG_LEVEL: [%s] => %d", categoryName, level);
}

// INSPECT_ELEMENT <factory_name>
//   Creates a temporary instance of the named element factory and prints
//   all of its properties.  Useful for inspecting elements that are not
//   currently part of any running pipeline.
static void cam2_dbg_inspect_element(const char *factoryName)
{
    if (!factoryName) {
        LOGE("INSPECT_ELEMENT: missing factory name");
        return;
    }

    GstElement *element = gst_element_factory_make(factoryName, "temp-inspect");
    if (!element) {
        LOGE("INSPECT_ELEMENT: factory '%s' not found", factoryName);
        return;
    }

    LOGI("INSPECT_ELEMENT: properties for factory '%s':", factoryName);
    gst_element_print_properties(element);
    gst_object_unref(element);
}

// SET_MAX_BITRATE <bps>
//   Forwards an upper bound to the Java encoder via
//   Camera2GstStreamer.setMaxBitRate(int).  Takes effect on the next stream
//   start.  Accepts a plain bps value or a K/M/G suffix (case-insensitive,
//   decimal multipliers), e.g. "2000000", "20M", "1500K", "8m".  0 = no cap.
static void cam2_dbg_set_max_bitrate(const char *bitrateStr)
{
    if (!bitrateStr) {
        LOGE("SET_MAX_BITRATE: missing bitrate value");
        return;
    }

    char *endPtr = NULL;
    double value = strtod(bitrateStr, &endPtr);
    if (endPtr == bitrateStr) {
        LOGE("SET_MAX_BITRATE: invalid bitrate '%s'", bitrateStr);
        return;
    }

    // Optional K/M/G suffix (decimal multipliers).
    switch (*endPtr) {
        case 'k': case 'K': value *= 1e3;  break;
        case 'm': case 'M': value *= 1e6;  break;
        case 'g': case 'G': value *= 1e9;  break;
        case '\0':                         break;
        default:
            LOGE("SET_MAX_BITRATE: invalid suffix in '%s' (use K/M/G)", bitrateStr);
            return;
    }

    if (value < 0) {
        LOGE("SET_MAX_BITRATE: negative bitrate '%s'", bitrateStr);
        return;
    }
    long bitrate = (long)value;

    if (!g_vm) {
        LOGE("SET_MAX_BITRATE: JavaVM not available");
        return;
    }

    JNIEnv *env = NULL;
    bool attached = false;
    jint getEnvResult = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvResult == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, NULL) != JNI_OK) {
            LOGE("SET_MAX_BITRATE: failed to attach current thread");
            return;
        }
        attached = true;
    } else if (getEnvResult != JNI_OK) {
        LOGE("SET_MAX_BITRATE: failed to get JNIEnv");
        return;
    }

    jclass cls = env->FindClass("com/crestron/streamout/Camera2GstStreamer");
    if (cls) {
        jmethodID mid = env->GetStaticMethodID(cls, "setMaxBitRate", "(I)V");
        if (mid) {
            env->CallStaticVoidMethod(cls, mid, (jint)bitrate);
            LOGI("SET_MAX_BITRATE: Camera2GstStreamer.setMaxBitRate(%ld)", bitrate);
        } else {
            LOGE("SET_MAX_BITRATE: setMaxBitRate(I)V method not found");
        }
        env->DeleteLocalRef(cls);
    } else {
        LOGE("SET_MAX_BITRATE: Camera2GstStreamer class not found");
    }

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }

    if (attached) {
        g_vm->DetachCurrentThread();
    }
}

// Parse a boolean argument: "true"/"false", "1"/"0", "on"/"off", "yes"/"no"
// (case-insensitive).  Returns 1 (true), 0 (false), or -1 on parse error.
static int cam2_dbg_parse_bool(const char *s)
{
    if (!s) return -1;
    if (!strcasecmp(s, "true")  || !strcasecmp(s, "1") ||
        !strcasecmp(s, "on")    || !strcasecmp(s, "yes")) return 1;
    if (!strcasecmp(s, "false") || !strcasecmp(s, "0") ||
        !strcasecmp(s, "off")   || !strcasecmp(s, "no"))  return 0;
    return -1;
}

// Calls a static void <methodName>(boolean) on Camera2GstStreamer with the
// parsed boolean argument.  Shared by the SET_RTCP_FEEDBACK / SET_PREFER_H265
// commands.  Takes effect on the next stream start.
static void cam2_dbg_call_bool_setter(const char *cmdName,
                                      const char *methodName,
                                      const char *valStr)
{
    if (!valStr) {
        LOGE("%s: missing boolean value (use true/false)", cmdName);
        return;
    }

    int parsed = cam2_dbg_parse_bool(valStr);
    if (parsed < 0) {
        LOGE("%s: invalid boolean '%s' (use true/false)", cmdName, valStr);
        return;
    }
    jboolean value = parsed ? JNI_TRUE : JNI_FALSE;

    if (!g_vm) {
        LOGE("%s: JavaVM not available", cmdName);
        return;
    }

    JNIEnv *env = NULL;
    bool attached = false;
    jint getEnvResult = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvResult == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, NULL) != JNI_OK) {
            LOGE("%s: failed to attach current thread", cmdName);
            return;
        }
        attached = true;
    } else if (getEnvResult != JNI_OK) {
        LOGE("%s: failed to get JNIEnv", cmdName);
        return;
    }

    jclass cls = env->FindClass("com/crestron/streamout/Camera2GstStreamer");
    if (cls) {
        jmethodID mid = env->GetStaticMethodID(cls, methodName, "(Z)V");
        if (mid) {
            env->CallStaticVoidMethod(cls, mid, value);
            LOGI("%s: Camera2GstStreamer.%s(%s)", cmdName, methodName,
                 parsed ? "true" : "false");
        } else {
            LOGE("%s: %s(Z)V method not found", cmdName, methodName);
        }
        env->DeleteLocalRef(cls);
    } else {
        LOGE("%s: Camera2GstStreamer class not found", cmdName);
    }

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }

    if (attached) {
        g_vm->DetachCurrentThread();
    }
}

// Calls a static void <methodName>(int) on Camera2GstStreamer with the given
// value.  Shared by integer-valued setters such as SET_STREAM_MODE.
static void cam2_dbg_call_int_setter(const char *cmdName,
                                     const char *methodName,
                                     jint value)
{
    if (!g_vm) {
        LOGE("%s: JavaVM not available", cmdName);
        return;
    }

    JNIEnv *env = NULL;
    bool attached = false;
    jint getEnvResult = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvResult == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, NULL) != JNI_OK) {
            LOGE("%s: failed to attach current thread", cmdName);
            return;
        }
        attached = true;
    } else if (getEnvResult != JNI_OK) {
        LOGE("%s: failed to get JNIEnv", cmdName);
        return;
    }

    jclass cls = env->FindClass("com/crestron/streamout/Camera2GstStreamer");
    if (cls) {
        jmethodID mid = env->GetStaticMethodID(cls, methodName, "(I)V");
        if (mid) {
            env->CallStaticVoidMethod(cls, mid, value);
            LOGI("%s: Camera2GstStreamer.%s(%d)", cmdName, methodName, value);
        } else {
            LOGE("%s: %s(I)V method not found", cmdName, methodName);
        }
        env->DeleteLocalRef(cls);
    } else {
        LOGE("%s: Camera2GstStreamer class not found", cmdName);
    }

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }

    if (attached) {
        g_vm->DetachCurrentThread();
    }
}

// SET_STREAM_MODE <0|1|2 | name>
//   Forwards the stream mode to Camera2GstStreamer.setStreamMode(int).  Accepts
//   a numeric ordinal (0 = VIDEO_ONLY, 1 = AUDIO_ONLY, 2 = VIDEO_AND_AUDIO_BOTH)
//   or a (case-insensitive) name.  Takes effect on the next stream start.
static void cam2_dbg_set_stream_mode(const char *modeStr)
{
    if (!modeStr) {
        LOGE("SET_STREAM_MODE: missing mode "
             "(0=VIDEO_ONLY, 1=AUDIO_ONLY, 2=VIDEO_AND_AUDIO_BOTH)");
        return;
    }

    int mode = -1;
    char *endPtr = NULL;
    long n = strtol(modeStr, &endPtr, 10);
    if (*endPtr == '\0') {
        mode = (int)n;                                  // pure numeric ordinal
    } else if (!strcasecmp(modeStr, "VIDEO_ONLY") ||
               !strcasecmp(modeStr, "VIDEO")) {
        mode = 0;
    } else if (!strcasecmp(modeStr, "AUDIO_ONLY") ||
               !strcasecmp(modeStr, "AUDIO")) {
        mode = 1;
    } else if (!strcasecmp(modeStr, "VIDEO_AND_AUDIO_BOTH") ||
               !strcasecmp(modeStr, "BOTH") ||
               !strcasecmp(modeStr, "AV")) {
        mode = 2;
    }

    if (mode < 0 || mode > 2) {
        LOGE("SET_STREAM_MODE: invalid mode '%s' "
             "(use 0/1/2 or VIDEO_ONLY/AUDIO_ONLY/VIDEO_AND_AUDIO_BOTH)", modeStr);
        return;
    }

    cam2_dbg_call_int_setter("SET_STREAM_MODE", "setStreamMode", (jint)mode);
}

// SET_AUDIO_SAMPLE_RATE <Hz>
//   Forwards the audio capture sample rate to
//   Camera2GstStreamer.setAudioSampleRate(int).  Independent of the channel
//   count, so any rate/channel combination can be tested (e.g. 48000 mono).
//   Typical values: 44100, 48000.  Takes effect on the next stream start.
static void cam2_dbg_set_audio_sample_rate(const char *rateStr)
{
    if (!rateStr) {
        LOGE("SET_AUDIO_SAMPLE_RATE: missing rate (e.g. 44100 or 48000)");
        return;
    }

    char *endPtr = NULL;
    long rate = strtol(rateStr, &endPtr, 10);
    if (*endPtr != '\0' || rate <= 0) {
        LOGE("SET_AUDIO_SAMPLE_RATE: invalid rate '%s'", rateStr);
        return;
    }

    cam2_dbg_call_int_setter("SET_AUDIO_SAMPLE_RATE", "setAudioSampleRate", (jint)rate);
}

// SET_AUDIO_CHANNELS <1|2>
//   Forwards the audio channel count to
//   Camera2GstStreamer.setAudioChannelCount(int).  Independent of the sample
//   rate.  1 = mono, 2 = stereo.  Takes effect on the next stream start.
static void cam2_dbg_set_audio_channels(const char *chStr)
{
    if (!chStr) {
        LOGE("SET_AUDIO_CHANNELS: missing channel count (1=mono, 2=stereo)");
        return;
    }

    char *endPtr = NULL;
    long ch = strtol(chStr, &endPtr, 10);
    if (*endPtr != '\0' || (ch != 1 && ch != 2)) {
        LOGE("SET_AUDIO_CHANNELS: invalid channels '%s' (use 1=mono or 2=stereo)", chStr);
        return;
    }

    cam2_dbg_call_int_setter("SET_AUDIO_CHANNELS", "setAudioChannelCount", (jint)ch);
}

// DUMP_ALL_CONFIG  (no args)
//   Calls Camera2GstStreamer.getConfigSummary() and logs the returned multi-line
//   summary of every externally-settable static setting (the values the SET_*
//   commands change).  Reports the *next-start intent* (what the next stream
//   start will use), NOT the live element state – use PRINT_ELEMENT_PROPERTY for
//   the running pipeline.
static void cam2_dbg_dump_all_config(void)
{
    if (!g_vm) {
        LOGE("DUMP_ALL_CONFIG: JavaVM not available");
        return;
    }

    JNIEnv *env = NULL;
    bool attached = false;
    jint getEnvResult = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvResult == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, NULL) != JNI_OK) {
            LOGE("DUMP_ALL_CONFIG: failed to attach current thread");
            return;
        }
        attached = true;
    } else if (getEnvResult != JNI_OK) {
        LOGE("DUMP_ALL_CONFIG: failed to get JNIEnv");
        return;
    }

    jclass cls = env->FindClass("com/crestron/streamout/Camera2GstStreamer");
    if (cls) {
        jmethodID mid = env->GetStaticMethodID(cls, "getConfigSummary",
                                               "()Ljava/lang/String;");
        if (mid) {
            jstring jsummary = (jstring)env->CallStaticObjectMethod(cls, mid);
            if (jsummary) {
                const char *summary = env->GetStringUTFChars(jsummary, NULL);
                if (summary) {
                    LOGI("DUMP_ALL_CONFIG:\n%s", summary);
                    env->ReleaseStringUTFChars(jsummary, summary);
                } else {
                    LOGE("DUMP_ALL_CONFIG: GetStringUTFChars failed");
                }
                env->DeleteLocalRef(jsummary);
            } else {
                LOGE("DUMP_ALL_CONFIG: getConfigSummary() returned null");
            }
        } else {
            LOGE("DUMP_ALL_CONFIG: getConfigSummary()Ljava/lang/String; not found");
        }
        env->DeleteLocalRef(cls);
    } else {
        LOGE("DUMP_ALL_CONFIG: Camera2GstStreamer class not found");
    }

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }

    if (attached) {
        g_vm->DetachCurrentThread();
    }
}

// SET_AUDIO_LATENCY_DEBUG <true|false>
//   Toggles ALL audio-latency diagnostic logging on/off in one shot:
//     • native "Opus encode latency" pad-probe print (opusenc sink->src compute)
//     • native "[A/V sync]" / "[EMA init]" PTS-correction prints
//     • Java   "Opus push latency" print (capture-start->appsrc)
//   LOGGING ONLY — the encode-probe stats, the EMA PTS correction, and audio
//   capture/push all keep running; this just silences the periodic logs.
//   The native half is set directly (no JNI); the Java half goes through
//   Camera2GstStreamer.setAudioLatencyDebug(boolean).
static void cam2_dbg_set_audio_latency_debug(const char *valStr)
{
    int parsed = cam2_dbg_parse_bool(valStr);
    if (parsed < 0) {
        LOGE("SET_AUDIO_LATENCY_DEBUG: invalid boolean '%s' (use true/false)",
             valStr ? valStr : "(null)");
        return;
    }
    // Native side: encode probe + A/V sync prints (direct call, no JNI).
    Cam2Streamer_SetAudioLatencyDebug(parsed);
    // Java side: "Opus push latency" print.
    cam2_dbg_call_bool_setter("SET_AUDIO_LATENCY_DEBUG", "setAudioLatencyDebug", valStr);
}

// DUMP_PIPELINE_GRAPH  (no args)
//   Triggers an on-demand pipeline dump: writes a Graphviz .dot graph of the
//   live pipeline AND logs the full element/property hierarchy to logcat.
//   Delegates to nativeDumpPipelineNow() in camera2_gst_streamer.cpp, which
//   takes the cached full pipeline ref under s_lock; if no client has played
//   yet it logs a "no pipeline available" warning.  That function ignores its
//   JNIEnv*/jclass arguments, so nullptr is passed directly (no JNI round-trip).
static void cam2_dbg_dump_pipeline_graph(void)
{
    Java_com_crestron_streamout_Camera2GstStreamer_nativeDumpPipelineNow(nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
void cam2_streamer_debug(char *cmd_cstring)
{
    if (!cmd_cstring) {
        LOGE("cam2_streamer_debug: null command string");
        return;
    }

    LOGI("cam2_streamer_debug: %s", cmd_cstring);

    // Skip the prefix token (e.g. "STREAMOUT_CAM2") to match the calling
    // convention of jni_rtsp_server_debug().
    char *tok = strtok(cmd_cstring, ", ");
    if (tok == NULL) {
        cam2_streamer_debug_printHelp();
        return;
    }

    tok = strtok(NULL, ", ");
    if (tok == NULL) {
        cam2_streamer_debug_printHelp();
        return;
    }

    LocalConvertToUpper(tok);
    int command = cam2_streamer_debug_getIndex(tok);
    LOGI("cam2_streamer_debug: command index = %d", command);

    switch (command) {
        case CAM2_DBG_PRINT_ELEMENT_PROPERTY:
        {
            const char *elementName = strtok(NULL, ", ");
            cam2_dbg_print_element_property(elementName);
            break;
        }
        case CAM2_DBG_SET_CATEGORY_DEBUG_LEVEL:
        {
            const char *categoryName = strtok(NULL, ", ");
            const char *levelStr     = (categoryName != NULL) ? strtok(NULL, ", ") : NULL;
            cam2_dbg_set_category_debug_level(categoryName, levelStr);
            break;
        }
        case CAM2_DBG_INSPECT_ELEMENT:
        {
            const char *factoryName = strtok(NULL, ", ");
            cam2_dbg_inspect_element(factoryName);
            break;
        }
        case CAM2_DBG_SET_MAX_BITRATE:
        {
            const char *bitrateStr = strtok(NULL, ", ");
            cam2_dbg_set_max_bitrate(bitrateStr);
            break;
        }
        case CAM2_DBG_SET_RTCP_FEEDBACK:
        {
            const char *valStr = strtok(NULL, ", ");
            cam2_dbg_call_bool_setter("SET_RTCP_FEEDBACK", "setRtcpFeedback", valStr);
            break;
        }
        case CAM2_DBG_SET_PREFER_H265:
        {
            const char *valStr = strtok(NULL, ", ");
            cam2_dbg_call_bool_setter("SET_PREFER_H265", "setPreferH265", valStr);
            break;
        }
        case CAM2_DBG_SET_STREAM_MODE:
        {
            const char *modeStr = strtok(NULL, ", ");
            cam2_dbg_set_stream_mode(modeStr);
            break;
        }
        case CAM2_DBG_SET_AUDIO_SAMPLE_RATE:
        {
            const char *rateStr = strtok(NULL, ", ");
            cam2_dbg_set_audio_sample_rate(rateStr);
            break;
        }
        case CAM2_DBG_SET_AUDIO_CHANNELS:
        {
            const char *chStr = strtok(NULL, ", ");
            cam2_dbg_set_audio_channels(chStr);
            break;
        }
        case CAM2_DBG_SET_USE_OPUS:
        {
            // SET_USE_OPUS <0|1|true|false> – switch the audio codec between AAC
            // (default) and Opus.  Forwards to Camera2GstStreamer.setUseOpus(bool),
            // which also forces the sample rate to 48000 when enabling Opus.
            // Takes effect on the next stream start.
            const char *valStr = strtok(NULL, ", ");
            cam2_dbg_call_bool_setter("SET_USE_OPUS", "setUseOpus", valStr);
            break;
        }
        case CAM2_DBG_SET_AUDIO_LATENCY_DEBUG:
        {
            const char *valStr = strtok(NULL, ", ");
            cam2_dbg_set_audio_latency_debug(valStr);
            break;
        }
        case CAM2_DBG_DUMP_ALL_CONFIG:
        {
            cam2_dbg_dump_all_config();
            break;
        }
        case CAM2_DBG_DUMP_PIPELINE_GRAPH:
        {
            cam2_dbg_dump_pipeline_graph();
            break;
        }

        default:
        {
            LOGE("cam2_streamer_debug: unknown command [%s]", tok);
            cam2_streamer_debug_printHelp();
            break;
        }
    }
}
