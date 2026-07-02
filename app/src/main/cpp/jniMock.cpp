#include <ctype.h>

#include "cregstplay.h"

// #include "csioCommonShare.h"
// #include "gstreamer_test.h"
CustomStreamOutData *CresStreamOutDataDB = NULL;

extern "C" void LocalConvertToUpper(char *str);

void csio_jni_recoverTxrxService(){}
void csio_jni_sendCameraStopFb(){}
void csio_jni_recoverWC_StreamOut() {}
void streamout_status_fb(int stream_id, int status_code, const char* info);

#define LOG_TAG "strmout_jnimock"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

void LocalConvertToUpper(char *str)
{
    char *TmpPtr;

    for (TmpPtr = str; *TmpPtr != 0; TmpPtr++)
    {
        *TmpPtr = toupper(*TmpPtr);
        if ( *TmpPtr == ' ')
            break;
    }
}


// Forward declarations for Java bridge
void java_stream_status(int stream_id, int status_code, const char* info);



void streamout_status_fb(int id, int status_code, const char* info)
{
    const char* safe_info = (info != nullptr) ? info : "";
    LOGI("streamout_status_fb called with id=%d, status_code=%d, info=%s", id, status_code, safe_info);
    // Call Java bridge to notify gRPC clients
    //char info[64];
    //snprintf(info, sizeof(info), "Stream status update, id=%d", id);
    java_stream_status(id, status_code, safe_info);
}