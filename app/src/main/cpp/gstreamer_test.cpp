#include <gtest/gtest.h>
#include <android/log.h>

#include <gst/gst.h>
#include <jni.h>
#include "../../../../../cresStreamOut.h"

#define LOG_TAG "strmout_gst_test"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

TEST(GStreamerTest, CheckInit) {
    //gst_init(NULL, NULL);
    LOGE("==CheckInit just to check now==gst_is_initialized[%d]=======\r\n",gst_is_initialized());

    ASSERT_TRUE(gst_is_initialized());

    // LOGE("==CheckInit gst_debug_set_default_threshold to debug========\r\n");
    // gst_debug_set_default_threshold(GST_LEVEL_DEBUG);
    GstElement *ele = gst_element_factory_make("queue",NULL);

    LOGE("==CheckInit gst_element_factory_create queue[%p]=======\r\n",ele);
    if (ele) {
        gst_object_unref(ele);
    } else {
        LOGE("queue element creation failed\n");
    }

    ele = gst_element_factory_make("ahcsrc",NULL);
    LOGE("==CheckInit gst_element_factory_create ahcsrc [%p]=======\r\n",ele);
    if (ele) {
        gst_object_unref(ele);
    } else {
        LOGE("ahcsrc element not found. Dumping plugin info and GST env for diagnosis...\n");
        LOGE("GST_PLUGIN_PATH=%s", getenv("GST_PLUGIN_PATH"));
        LOGE("GST_REGISTRY=%s", getenv("GST_REGISTRY"));
    }
    
    ele = gst_element_factory_make("tinyalsasrc",NULL);
    LOGE("==CheckInit gst_element_factory_create tinyalsasrc [%p]=======\r\n",ele);
    if (ele)
        gst_object_unref(ele);

    ele = gst_element_factory_make("tinyalsasink",NULL);   
    LOGE("==CheckInit gst_element_factory_create tinyalsasink [%p]=======\r\n",ele);
    if (ele)
        gst_object_unref(ele);
    
    // Product_Information* info = product_info();
    // LOGE("==CheckInit info[%p]=======\r\n",info);
    LOGE("==CheckInit gst_version_string: %s=======\r\n",gst_version_string());

    LOGE("==CheckInit exit======\r\n");
}
TEST(GStreamerTest, checkStreamoutInit) {
    //gst_init(NULL, NULL);
    LOGE("==checkStreamoutInit just to check StreamoutProjectInit=======\r\n");

    StreamoutProjectInit(eStreamoutMode::STREAMOUT_MODE_CAMERA);

    ASSERT_TRUE(true);   
    
    int temp = 0;
    while (temp++ < 5)
    {
        usleep(100000);//100ms
        LOGE("\r\ncheckStreamoutInit sleep 100ms\r\n");
    }

    LOGE("\r\ncalling StreamoutProjectDeInit\r\n");
    StreamoutProjectDeInit();

    {
        // char *filePath = "/data/local/tmp/rtsp_pipeline";

        // FILE *file = fopen(filePath, "w");
        // if (file != NULL)
        // {
        //     fprintf(file, "%s", "videotestsrc is-live=true name=vidtest1080 ! video/x-raw, width=1280,height=720,framerate=30/1 ! videoconvert ! queue ! amcvidenc-omxqcomvideoencoderavc i-frame-interval=1 name=encoder ! h264parse ! queue ! rtph264pay config-interval=-1 name=pay0 pt=96");
        //     fclose(file);
        //     LOGE("Writing graph file - %s", filePath);
        // }
        // else
        // {
        //     LOGE("Error writing graph file - %s: %s", filePath, strerror(errno));
        // }



        const char *envPath = getenv("RTSP_PIPELINE_PATH");
        const char *primary = envPath ? envPath : "/sdcard/rtsp_server_pipeline";
        const char *localFileName = "rtsp_pipeline";
        char fullPath[512];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", primary, localFileName);
        const char *pipeline = "videotestsrc is-live=true name=vidtest1080 ! video/x-raw, width=1280,height=720,framerate=30/1 ! videoconvert ! queue ! amcvidenc-omxqcomvideoencoderavc i-frame-interval=1 name=encoder ! h264parse ! queue ! rtph264pay config-interval=-1 name=pay0 pt=96";

        LOGE("Writing graph file - %s", fullPath);

        FILE *file = fopen(fullPath, "w");
        if (file) {
            fprintf(file, "%s", pipeline);
            fclose(file);
            LOGE("Successfully wrote pipeline to file - %s", fullPath);
        } else {
            LOGE("Error writing pipeline file - %s: %s", fullPath, strerror(errno));
        }
    }

    LOGE("==checkStreamoutInit exit======\r\n");
}

TEST(GStreamerTest, checkStreamoutPortAndStart) {
    //gst_init(NULL, NULL);
    LOGE("==checkStreamoutPortAndStart just to check StreamoutProjectInit=======\r\n");

    StreamoutProjectInit(eStreamoutMode::STREAMOUT_MODE_CAMERA);

    ASSERT_TRUE(true);   
    
    // gst_debug_set_threshold_for_name("ahcsrc", GST_LEVEL_DEBUG);

    Streamout_SetPort("8555");
    Streamout_Start(0);

    // int temp = 0;
    // #define SLEEP_TIME_SECONDS 10
    // #define WAIT_LOOP_COUNT (10 * 60 / SLEEP_TIME_SECONDS) // 10 minutes total
    // // #define WAIT_LOOP_COUNT (1 * 60 / SLEEP_TIME_SECONDS) // 1 minutes total

    // while (temp++ < WAIT_LOOP_COUNT)
    // {
    //     sleep(SLEEP_TIME_SECONDS); 
    //     LOGE("\r\ncheckStreamoutInit sleep %d seconds----------------------------------\r\n", SLEEP_TIME_SECONDS);
    // }

    // Streamout_Stop(0);

    // sleep(5); 

    // LOGE("calling StreamoutProjectDeInit\r\n");
    // StreamoutProjectDeInit();

    LOGE("==checkStreamoutPortAndStart exit======\r\n");
}


TEST(GStreamerTest, checkStreamoutPlayVideo) {
    LOGE("==checkStreamoutPlayVideo just to check StreamoutProjectInit=======\r\n");

    //check video file first
    const char *envPath = getenv("RTSP_PIPELINE_PATH");
    const char *primary = envPath ? envPath : "/sdcard/rtsp_server_pipeline";
    const char *localFileName = "IRobot.mp4";
    char fullPath[512];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", primary, localFileName);
    // const char *pipeline = "videotestsrc is-live=true name=vidtest1080 ! video/x-raw, width=1280,height=720,framerate=30/1 ! videoconvert ! queue ! amcvidenc-omxqcomvideoencoderavc i-frame-interval=1 name=encoder ! h264parse ! queue ! rtph264pay config-interval=-1 name=pay0 pt=96";

    char *pipeline = "filesrc location=/data/user/0/com.crestron.streamout/files/IRobot.mp4  ! "
                     "qtdemux name=demux  demux. ! queue ! h264parse ! "
                     "rtph264pay name=pay0 pt=96 demux. ! queue ! "
                     "aacparse !  rtpmp4apay name=pay1 pt=97";

    LOGE("read video file from file - %s", fullPath);
    LOGE("Create pipeline from - %s", pipeline);

    FILE *file = fopen(fullPath, "r");
    if (file) {
        fclose(file);

        LOGE("Successfully open video file - %s", fullPath);
    
        StreamoutProjectInit(eStreamoutMode::STREAMOUT_MODE_CAMERA);

        ASSERT_TRUE(true);   
        
        // gst_debug_set_threshold_for_name("ahcsrc", GST_LEVEL_DEBUG);

        Streamout_SetPort("8555");
        Streamout_SetStreamPipeline((char *)pipeline);
        Streamout_Start(0);
    
    } else {
        LOGE("Error opening video file - %s: %s", fullPath, strerror(errno));
    }

    LOGE("==checkStreamoutPlayVideo exit======\r\n");
}
TEST(GStreamerTest, checkStreamoutColorbar) {
    LOGE("==checkStreamoutColorbar ===3-10-2026====\r\n");

    char *filePath = "/dev/shm/rtsp_server_pipeline";

    FILE *file = fopen(filePath, "w");
    if (file != NULL)
    {
        fprintf(file, "%s", "videotestsrc is-live=true name=vidtest1080 ! video/x-raw, width=1280,height=720,framerate=30/1 ! videoconvert ! queue ! amcvidenc-omxqcomvideoencoderavc i-frame-interval=1 name=encoder ! h264parse ! queue ! rtph264pay config-interval=-1 name=pay0 pt=96");
        fclose(file);
        CSIO_LOG(eLogLevel_info, "Writing graph file - %s", filePath);
    }
    else
    {
        CSIO_LOG(eLogLevel_info, "Error writing graph file - %s: %s", filePath, strerror(errno));
    }

    StreamoutProjectInit(eStreamoutMode::STREAMOUT_MODE_CAMERA);

    Streamout_SetPort("8555");
    Streamout_SetStreamPipeline("videotestsrc is-live=1 ! "
                                "video/x-raw,width=640,height=480,framerate=30/1 ! "
                                "videoconvert ! amcvidenc-omxqcomvideoencoderavc i-frame-interval=1 ! "
                                "h264parse ! rtph264pay name=pay0 pt=96");
    Streamout_Start(0);
    // sleep(2);

    // Streamout_Stop(0);
    // sleep(2);
    // StreamoutProjectDeInit();

    LOGE("==checkStreamoutColorbar exit======\r\n");
}

extern "C" JNIEXPORT jint JNICALL
Java_com_crestron_streamout_StreamOutSvcCtrl_runNativeTests(JNIEnv* env, jobject thiz) {
    // allow the caller to limit which tests execute by setting the
    // GTEST_FILTER environment variable.  google-test will honor either
    // `--gtest_filter` on the command line or the GTEST_FILTER env var;
    // we detect the latter and synthesize an argv entry so that the
    // standard InitGoogleTest machinery works unchanged.
    const char *filter = getenv("GTEST_FILTER");
    int argc = 1;
    char *argv[2] = {(char *)"gtest_android", nullptr};

    LOGE("GTEST_FILTER='%s' got filter: %p\n", filter, (void *)filter);

    if (filter && *filter) {
        static char argbuf[256];
        snprintf(argbuf, sizeof(argbuf), "--gtest_filter=%s", filter);
        argv[1] = argbuf;
        argc = 2;
        LOGE("GTEST_FILTER='%s' supplied, applying filter\n", filter);
    }

    testing::InitGoogleTest(&argc, argv);
    LOGE("==runNativeTests===GST_DEBUG set to: %s========\r\n", getenv("GST_DEBUG"));
    LOGE("=====starting RUN_ALL_TESTS (argc=%d)========\r\n", argc);
    return RUN_ALL_TESTS();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_crestron_streamout_StreamOutSvcCtrl_JNI_1OnLoad(JNIEnv* env, void* reserved) {
    LOGE("=====JNI_OnLoad========\r\n");

    char temp[256];
    
    LOGE("==LOGE===GStreamer version: %s========\r\n", gst_version_string());

    snprintf(temp, sizeof(temp), "GST_DEBUG: %s", "ahcsrc:5"); 
    setenv("GST_DEBUG", temp, 1);

    LOGE("==JNI_OnLoad===GST_DEBUG set to: %s========\r\n", getenv("GST_DEBUG"));


    return JNI_VERSION_1_6;
}
extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_StreamOutSvcCtrl_nativeSetEnv(JNIEnv* env, jobject thiz,
                                                       jstring jkey, jstring jvalue) {

    LOGE("=====nativeSetEnv========\r\n");

    const char *key = env->GetStringUTFChars(jkey, nullptr);
    const char *value = env->GetStringUTFChars(jvalue, nullptr);
    setenv(key, value, 1);  // 1 = overwrite
    LOGE("nativeSetEnv: %s=%s\n", key, value);
    env->ReleaseStringUTFChars(jkey, key);
    env->ReleaseStringUTFChars(jvalue, value);
}
//=================copy from GstreamBase_jni.cpp===========================
void listGStreamerPlugins(bool rescan = false)
{
    GstRegistry *registry = gst_registry_get();

    assert(registry);

    if(rescan)
    {
        if(!gst_update_registry())
        {
            LOGE("registry updated failed");
        }
    }

    GList *plugins = gst_registry_get_plugin_list(registry);
    int count = 0;

    assert(plugins);

    for (GList *i = plugins; i; i = g_list_next(i))
    {
        GstPlugin *curr = static_cast<GstPlugin *>(i->data);

        LOGE(            
            "%s blacklisted %d",
            gst_plugin_get_name(curr),
            GST_OBJECT_FLAG_IS_SET(curr, GST_PLUGIN_FLAG_BLACKLISTED));
        count++;
    }

    gst_plugin_list_free(plugins);
}

extern "C" JNIEXPORT void JNICALL
Java_com_crestron_streamout_GstreamBase_postGStreamerInit(
    JNIEnv *env, jobject thiz)
{
    LOGE("postGStreamerInit GST_DEBUG %s", getenv("GST_DEBUG"));
    LOGE("postGStreamerInit is_initialized %d", gst_is_initialized());
    LOGE("postGStreamerInit debug_is_active %d", gst_debug_is_active());
    LOGE("postGStreamerInit debug_threshold %d", gst_debug_get_default_threshold());
    LOGE("postGStreamerInit version  %s", gst_version_string());
    LOGE("postGStreamerInit GST_PLUGIN_PATH %s", getenv("GST_PLUGIN_PATH"));
    LOGE("postGStreamerInit GST_REGISTRY %s", getenv("GST_REGISTRY"));
    GST_INFO("validate GST_INFO env %p thiz %p", env, thiz);
    GST_DEBUG("validate GST_DEBUG env %p thiz %p", env, thiz);
    GST_LOG("validate GST_LOG env %p thiz %p", env, thiz);
    GST_TRACE("validate GST_TRACE env %p thiz %p", env, thiz);
    listGStreamerPlugins();
    listGStreamerPlugins(true);

    {
        char temp[256];

        snprintf(temp, sizeof(temp), "GST_DEBUG: %s", "*:1"); 
        setenv("GST_DEBUG", temp, 1);
        LOGE("==postGStreamerInit===GST_DEBUG set to: %s========\r\n", getenv("GST_DEBUG"));
    }
    
    // {
    //     char temp[256];

    //     snprintf(temp, sizeof(temp), "GST_DEBUG: %s", "*:1,ahcsrc:5"); 
    //     setenv("GST_DEBUG", temp, 1);
    //     LOGE("==postGStreamerInit===GST_DEBUG set back to: %s========\r\n", getenv("GST_DEBUG"));
    // }

    LOGE("postGStreamerInit calling listGStreamerPlugins after registering androidmedia plugin");
    listGStreamerPlugins();
    listGStreamerPlugins(true);
}
//===============end of copy from GstreamBase_jni.cpp===========================
