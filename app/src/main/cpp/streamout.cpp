
#include <jni.h>
#include <android/log.h>
#include <string>
#include "csioCommonShare.h"

#define LOG_TAG "strmout_streamout"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

JavaVM* g_vm = nullptr;
jobject g_classLoaderObj = nullptr; // Global ref to Java class loader provider (e.g., Activity or Context)
extern void setup_product_info(unsigned long productId);
extern "C" void cam2_streamer_debug(char *cmd_cstring);

// Hold global references to class and method IDs
static jclass g_StrmOutGrpcServer_class = nullptr;
static jmethodID g_streamStatus_method = nullptr;
static jclass g_streamStatusEnum_class = nullptr;
static jmethodID g_streamStatusEnum_forNumber_method = nullptr;

jclass g_streamOutSvcCtrlClass = nullptr;
jmethodID g_runNotificationThreadMid = nullptr;
jobject g_streamOutSvcCtrlObj = nullptr;

static std::string g_rtspServerPort = "8555"; // Default RTSP server port

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_vm = vm;

    //TODO: need to fix product_info() function later!!!
    setup_product_info(SYSTEM_TSX80);//0x6F00(28416)
    // setup_product_info(0x6000);
    // setup_product_info(0x6F00);
    // setup_product_info(0x8000);
    //LOGI("JNI_OnLoad 5-11-2026 9:40Am");

    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        LOGE("JNI_OnLoad: Failed to get JNIEnv");
        return JNI_ERR;
    }

    jclass localClass = env->FindClass("com/crestron/streamout/StreamOutSvcCtrl");
    if (!localClass) {
        LOGE("JNI_OnLoad: Failed to find StreamOutSvcCtrl class");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        return JNI_ERR;
    }

    g_streamOutSvcCtrlClass = reinterpret_cast<jclass>(env->NewGlobalRef(localClass));
    env->DeleteLocalRef(localClass);
    if (!g_streamOutSvcCtrlClass) {
        LOGE("JNI_OnLoad: Failed to create global ref for StreamOutSvcCtrl class");
        return JNI_ERR;
    }

    g_runNotificationThreadMid = env->GetMethodID(g_streamOutSvcCtrlClass, "RunNotificationThread", "()V");
    if (!g_runNotificationThreadMid) {
        LOGE("JNI_OnLoad: Failed to find RunNotificationThread method");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        env->DeleteGlobalRef(g_streamOutSvcCtrlClass);
        g_streamOutSvcCtrlClass = nullptr;
        return JNI_ERR;
    }

    LOGI("JNI_OnLoad: Cached StreamOutSvcCtrl.RunNotificationThread: %p",g_runNotificationThreadMid);

    //Find the StrmOutGrpcServer class and hold a global reference
    localClass = env->FindClass("com/crestron/streamout/StrmOutGrpcServer");
    if (!localClass) {
        LOGE("JNI_OnLoad: Failed to find StrmOutGrpcServer class");
        return JNI_ERR;
    }

    g_StrmOutGrpcServer_class = reinterpret_cast<jclass>(env->NewGlobalRef(localClass));
    env->DeleteLocalRef(localClass);
    if (!g_StrmOutGrpcServer_class) {
        LOGE("JNI_OnLoad: Failed to create global ref for StrmOutGrpcServer class");
        return JNI_ERR;
    }

    g_streamStatus_method = env->GetStaticMethodID(g_StrmOutGrpcServer_class, "streamStatusFromNative", "(ILstreamout/v1/GrpcStrOut$StreamStatus;Ljava/lang/String;)V");
    if (!g_streamStatus_method) {
        LOGE("JNI_OnLoad: Failed to get streamStatusFromNative method ID");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        env->DeleteGlobalRef(g_StrmOutGrpcServer_class);
        g_StrmOutGrpcServer_class = nullptr;
        return JNI_ERR;
    }
    LOGI("JNI_OnLoad: Cached StrmOutGrpcServer.streamStatusFromNative: %p", g_streamStatus_method);

    jclass enumLocalClass = env->FindClass("streamout/v1/GrpcStrOut$StreamStatus");
    if (!enumLocalClass) {
        LOGE("JNI_OnLoad: Failed to find StreamStatus enum class");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        env->DeleteGlobalRef(g_StrmOutGrpcServer_class);
        g_StrmOutGrpcServer_class = nullptr;
        return JNI_ERR;
    }

    g_streamStatusEnum_class = reinterpret_cast<jclass>(env->NewGlobalRef(enumLocalClass));
    env->DeleteLocalRef(enumLocalClass);
    if (!g_streamStatusEnum_class) {
        LOGE("JNI_OnLoad: Failed to create global ref for stream_status_enum class");
        env->DeleteGlobalRef(g_StrmOutGrpcServer_class);
        g_StrmOutGrpcServer_class = nullptr;
        return JNI_ERR;
    }

    g_streamStatusEnum_forNumber_method = env->GetStaticMethodID(
            g_streamStatusEnum_class,
            "forNumber",
            "(I)Lstreamout/v1/GrpcStrOut$StreamStatus;");
    if (!g_streamStatusEnum_forNumber_method) {
        LOGE("JNI_OnLoad: Failed to get stream_status_enum.forNumber method ID");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        env->DeleteGlobalRef(g_streamStatusEnum_class);
        g_streamStatusEnum_class = nullptr;
        env->DeleteGlobalRef(g_StrmOutGrpcServer_class);
        g_StrmOutGrpcServer_class = nullptr;
        return JNI_ERR;
    }
    LOGI("JNI_OnLoad: Cached stream_status_enum.forNumber: %p", g_streamStatusEnum_forNumber_method);

    return JNI_VERSION_1_6;
}


// JNI_OnUnload: Clean up global references
JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return;
    }

    // Call Java static method to shutdown gRPC server before cleaning up references
    if (g_StrmOutGrpcServer_class) {
        jmethodID shutdownMid = env->GetStaticMethodID(g_StrmOutGrpcServer_class, "shutdownGrpcServer", "()V");
        if (shutdownMid) {
            env->CallStaticVoidMethod(g_StrmOutGrpcServer_class, shutdownMid);
            LOGE("JNI_OnUnload: Called StrmOutGrpcServer.shutdownGrpcServer()");
        } else {
            LOGE("JNI_OnUnload: Failed to get shutdownGrpcServer method ID");
        }
    }

    if (g_streamOutSvcCtrlClass) {
        env->DeleteGlobalRef(g_streamOutSvcCtrlClass);
        g_streamOutSvcCtrlClass = nullptr;
    }
    if (g_streamOutSvcCtrlObj) {
        env->DeleteGlobalRef(g_streamOutSvcCtrlObj);
        g_streamOutSvcCtrlObj = nullptr;
    }
    if (g_StrmOutGrpcServer_class) {
        env->DeleteGlobalRef(g_StrmOutGrpcServer_class);
        g_StrmOutGrpcServer_class = nullptr;
    }
    if (g_classLoaderObj) {
        env->DeleteGlobalRef(g_classLoaderObj);
        g_classLoaderObj = nullptr;
    }
    if (g_streamStatusEnum_class) {
        env->DeleteGlobalRef(g_streamStatusEnum_class);
        g_streamStatusEnum_class = nullptr;
    }
    g_streamStatus_method = nullptr;
    g_streamStatusEnum_forNumber_method = nullptr;
    g_runNotificationThreadMid = nullptr;
}

// extern "C" JNIEXPORT void JNICALL
// Java_com_crestron_streamout_StreamOutSvcCtrl_nativeSetStreamoutPort(JNIEnv* env, jobject thiz, jstring jport) {
//     const char *port = env->GetStringUTFChars(jport, nullptr);
//     LOGI("nativeSetStreamoutPort called with port: %s", port);
//     Streamout_SetPort((char *)port);
//     g_rtspServerPort = std::string(port);
//     env->ReleaseStringUTFChars(jport, port);
// }

// extern "C" JNIEXPORT void JNICALL
// Java_com_crestron_streamout_StreamOutSvcCtrl_nativeSetStreamoutPipeline(JNIEnv* env, jobject thiz, jstring jpipeline) {
//     const char *pipeline = env->GetStringUTFChars(jpipeline, nullptr);
//     LOGI("nativeSetStreamoutPipeline called with pipeline: %s", pipeline);
//     Streamout_SetStreamPipeline((char *)pipeline);
//     env->ReleaseStringUTFChars(jpipeline, pipeline);
// }

// extern "C" JNIEXPORT void JNICALL
// Java_com_crestron_streamout_StreamOutSvcCtrl_nativeStreamoutStart(JNIEnv* env, jobject thiz, jint arg) {
        
//     LOGI("nativeStreamoutStart: get rtsp port and sending: %s", g_rtspServerPort.c_str());
//     Streamout_SetPort((char *)g_rtspServerPort.c_str());

//     LOGI("nativeStreamoutStart called with arg: %d", arg);
//     Streamout_Start(arg);
// }

// extern "C" JNIEXPORT void JNICALL
// Java_com_crestron_streamout_StreamOutSvcCtrl_nativeStreamoutStop(JNIEnv* env, jobject thiz, jint arg) {
//     LOGI("nativeStreamoutStop called with arg: %d", arg);
//     Streamout_Stop(arg);
// }

extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_StreamOutSvcCtrl_jniRtspServerDebug(JNIEnv* env, jobject thiz, jstring cmdString) {
    const char *cmd_cstring = env->GetStringUTFChars(cmdString, nullptr);
    LOGI("jniRtspServerDebug called with cmd_cstring: %s", cmd_cstring);

    // strtok() mutates its input, so give each dispatcher its own copy.
    char *cam2_copy = strdup(cmd_cstring ? cmd_cstring : "");
    cam2_streamer_debug(cam2_copy);
    free(cam2_copy);

    // jni_rtsp_server_debug((char *)cmd_cstring);
    env->ReleaseStringUTFChars(cmdString, cmd_cstring);
}

// extern "C" JNIEXPORT void JNICALL
// Java_com_crestron_streamout_StreamOutSvcCtrl_nativeStreamoutProjectInit(JNIEnv* env, jobject thiz, jint mode) {
//     LOGI("nativeStreamoutProjectInit called with mode: %d", mode);
//     StreamoutProjectInit(static_cast<eStreamoutMode>(mode));
// }

extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_StreamOutSvcCtrl_nativeSetProductID(JNIEnv* env, jobject thiz, jint id) {
    LOGI("nativeSetProductID called with id: %d", id);
    setup_product_info(static_cast<unsigned long>(id));
}

// extern "C" JNIEXPORT void JNICALL
// Java_com_crestron_streamout_StreamOutSvcCtrl_nativeStreamoutProjectDeInit(JNIEnv* env, jobject thiz) {
//     LOGI("nativeStreamoutProjectDeInit called");
//     StreamoutProjectDeInit();
// }


// extern "C" JNIEXPORT void JNICALL
// Java_com_crestron_streamout_StreamOutSvcCtrl_setNativePath(JNIEnv *env, jobject thiz, jstring path) {
//     const char *chars = env->GetStringUTFChars(path, nullptr);
//     // globalPath = std::string(chars); // Save it for later use

//     // LOGI("setNativePath: %s", globalPath.c_str());
//     env->ReleaseStringUTFChars(path, chars);
// }

// Bridge function to call Java static method for streamIsReady
#include <jni.h>
#include <android/log.h>

// Bridge function to call Java static method for streamStatusFromNative
void java_stream_status(int stream_id, int status_code, const char* info) {
    if (!g_vm) {
        LOGE("java_stream_status: g_vm is null");
        return;
    }
    if (!g_StrmOutGrpcServer_class || !g_streamStatus_method) {
        LOGE("java_stream_status: class or method ID not cached");
        return;
    }
    if (!g_streamStatusEnum_class || !g_streamStatusEnum_forNumber_method) {
        LOGE("java_stream_status: enum class or method ID not cached");
        return;
    }

    // A JNIEnv* is only valid on a JVM-attached thread.  Threads created by
    // Java (gRPC handlers, UI) are already attached and own their JNIEnv;
    // threads created by native code (GStreamer pipeline, pthreads) are not.
    // Attach only when this thread is currently detached, and remember whether
    // WE attached so we can detach on the way out.  Detaching only what we
    // attached avoids two bugs: (1) leaking JVM per-thread state (or aborting
    // on thread exit) when a native thread attaches but never detaches, and
    // (2) wrongly detaching a Java-owned thread that must stay attached.
    JNIEnv* env = nullptr;
    bool didAttach = false;
    jint getEnvRc = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvRc == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("java_stream_status: Failed to attach current thread");
            return;
        }
        didAttach = true;
    } else if (getEnvRc != JNI_OK) {
        LOGE("java_stream_status: GetEnv failed (rc=%d)", getEnvRc);
        return;
    }

    jobject enumValue = env->CallStaticObjectMethod(
            g_streamStatusEnum_class, g_streamStatusEnum_forNumber_method, status_code);
    if (enumValue) {
        const char* safeInfo = (info != nullptr) ? info : "";
        jstring jinfo = env->NewStringUTF(safeInfo);
        if (jinfo) {
            env->CallStaticVoidMethod(g_StrmOutGrpcServer_class, g_streamStatus_method,
                                      stream_id, enumValue, jinfo);
            env->DeleteLocalRef(jinfo);
            LOGI("java_stream_status called");
        } else {
            // NewStringUTF can fail (OOM / pending exception); clear it so we
            // don't leave an exception pending on the thread we may detach.
            LOGE("java_stream_status: NewStringUTF failed");
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
            }
        }
        env->DeleteLocalRef(enumValue);
    } else {
        LOGE("java_stream_status: Invalid stream_status_enum value: %d", status_code);
    }

    // Detach only if this call attached the thread (see note above).
    if (didAttach) {
        g_vm->DetachCurrentThread();
    }
}
