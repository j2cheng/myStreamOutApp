#include "csioCommonShare.h"

void csio_log(eLogLevel logLevel, const char *stringFormat, ...){}
void csio_logalways(eLogLevel logLevel, const char * categoryStr, const char * stringFormat, ...) {}
void csio_log_to_error_server(eLogLevel logLevel, const char *stringFormat, ...) {}
void csio_log_save_log_level(eLogLevel logLevel);
int csio_log_retreive_log_level() {return 0;}
void csio_ForceQuit() {}
void csio_createMultiControlMtx() {}
void csio_deleteMultiControlMtx() {}
void csio_getMultiControlMtx() {}
void csio_releaseMultiControlMtx() {}
int  csio_GetStreamingStatesCnt() {return 0;}

void csio_setup_product_info(int){}
int GetInPausedState(unsigned stream_id){ return 0; }
void SetInPausedState(unsigned stream_id, unsigned paused){}
void setup_product_info(unsigned long productId);

#define LOG_TAG "strmout_csioMock"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define CRESTRON_PRODUCT_TYPE unsigned long

Product_Information mockProductInfo = {};

static Product_Information Product_Information_Table [] =
{
    {
        SYSTEM_TSX80,                                       // product_type
        32002,                                              // iplink_reserve_join_number
        49306,                                              // iplink_tcp_port
        eHardwarePlatform_Qualcomm_TSX80,                         // hw_platform
        SMART_GRAPHICS_BITMASK_RUNS_SG | SMART_GRAPHICS_BITMASK_RUNS_BB,	// smart_graphics_bitmask
        GSTREAMER_BITMASK_GSTREAMER_1_8,                    // gstreamer_bitmask
        STATISTICS_BITMASK_ALWAYS_ON,                       // stream_statistics_bitmask
        0x00000001,                                         // stream_display_bitmask
        0x00000000,                                         // display_check_hdcp_bitmask
        ePauseCapability_PauseWithValves,                   // pause_capability
        true,	                                            // stop_on_messagepage
        4,                                                  // maximum_video_windows
        4,                                                  // maximum_local_video_windows
        3,                                                  // maximum stream_id
        3,                                                  // maximum_txrx_id
        {JOIN_RESPONSE_NO_SLOTS},                           // join_response_slots_digital - Initialize in CSIOProductSpecific.cpp
        {JOIN_RESPONSE_NO_SLOTS},                           // join_response_slots_analog - Initialize in CSIOProductSpecific.cpp
        {JOIN_RESPONSE_NO_SLOTS},                           // join_response_slots_serial - Initialize in CSIOProductSpecific.cpp												// join_response_slots_serial
        false,                                              // should_translate_slots
        {{0,0}},                                            // slot_translation_map - Initialize in CSIOProductSpecific.cpp
        0,                                                  // dm_input_cnt
        true,                                               // process_hdmi_in_audio
        "amcviddec-omxqcomvideodecoderavc",                 // video_decoder_string
        "",                                                 // mjpeg decoder string (use gstreamer sw decoder)
        "amcvidenc-omxqcomvideoencoderavc",					// video encoder string
        "amcviddec-omxqcomvideodecoderhevc",                // H265_decoder_string
        false,                                      	    // Proxy support is off
        DCP_MODE_NONE,                                      // No HDCP 2.x available
        48000,                                              // output_audio_samplerate
        0,                                                  // chroma_key_used is true
        false,                                              // hide_video_on_stop
        false,                                              // hasAutomaticInitiationMode
        false,                                              // Product has DSP
        false,                                              // doesProductHaveExternalHDMI
        1,                                                  //numberEthernetAdapters
        0,                                                  //primaryEthernetAdapter
        false,                                              // mapSettingsToSdcard
        false,                                              // restartStreamsOnStart
        false,                                              // doesProductHaveMiracast
        {{4096,2160,0},                                     // video resolution limits table
        {4096,2160,0},
        {4096,2160,0},
        {4096,2160,0}},
        false,                                              // enable AirMedia by default
        false,                                              // product has HDMI output port
        true                                                // does product support 4k airMedia video decode
    },
    {
			(CRESTRON_PRODUCT_TYPE)0 /* Default */,				// product_type
			32002,												// iplink_reserve_join_number
			49306,												// iplink_tcp_port
			eHardwarePlatform_Amlogic,							// hw_platform
			SMART_GRAPHICS_BITMASK_RUNS_SG | SMART_GRAPHICS_BITMASK_RUNS_BB, // smart_graphics_bitmask
			GSTREAMER_BITMASK_GSTREAMER_1_8,					// gstreamer_bitmask
			STATISTICS_BITMASK_ALWAYS_ON,						// stream_statistics_bitmask
			0x00000001,											// stream_display_bitmask
			0x00000000,											// display_check_hdcp_bitmask
			ePauseCapability_PauseWithValves,					// pause_capability
			true,												// stop_on_messagepage
			4,													// maximum_video_windows
			4,													// maximum_local_video_windows
			3,													// maximum stream_id
            3,                                                  // maximum_txrx_id
			{JOIN_RESPONSE_NO_SLOTS},							// join_response_slots_digital - Initialize in CSIOProductSpecific.cpp
			{JOIN_RESPONSE_NO_SLOTS},							// join_response_slots_analog - Initialize in CSIOProductSpecific.cpp
			{JOIN_RESPONSE_NO_SLOTS},							// join_response_slots_serial - Initialize in CSIOProductSpecific.cpp												// join_response_slots_serial
			false,												// should_translate_slots
			{{0,0}},											// slot_translation_map - Initialize in CSIOProductSpecific.cpp
			0,													// dm_input_cnt
			true, 												// process_hdmi_in_audio
			"xxx need to fill this in based on media_codecs.xml",                   // video decoder string
			"xxx need to fill this in based on media_codecs.xml",			   	 	// mjpeg decoder string
			"xxx need to fill this in based on media_codecs.xml",				    // video encoder string
			"xxx need to fill this in based on media_codecs.xml",	                // H265_decoder_string
			false,                                              // Proxy support is off
			DCP_MODE_NONE,										// No HDCP 2.x available
			48000,												// output_audio_samplerate
			1,                                                  // chroma_key_used is true
			false,												// hide_video_on_stop
			false,												// hasAutomaticInitiationMode
			false,												// Product has DSP
			false,												// doesProductHaveExternalHDMI
			1,													//numberEthernetAdapters
			0,													//primaryEthernetAdapter
			false,												// mapSettingsToSdcard
			false,												// restartStreamsOnStart
			false,                                               // doesProductHaveMiracast
            {{4096,2160,0},                                     // video resolution limits table
            {4096,2160,0},
            {4096,2160,0},
            {4096,2160,0}},
            false,                                              // enable AirMedia by default
			false,                                              // product has HDMI output port
            false                                               // does product support 4k airMedia video decode
	}
};

int wcVideoEncDumpEnable = 0;
int wcAudioEncDumpEnable  = 0;
int wcAudioStatsEnable  = 0;
int wcAudioStatsReset  = 0;
int wcJpegStatsEnable  = 0;
int wcJpegStatsReset  = 0;
int wcJpegRateControl  = 0;
int wcJpegDynamicFrameRateControl  = 0;
int wcVideoQueueMaxTime  = 0;
int wcVideoQueueMaxBuffers  = 0;
int wcShowVideoQueueOverruns  = 0;
bool wcFrameTrackerEnable  = false;
int wcShowFrameLatency  = 0;


int wcJpegRateBitsPerMsec = 0;

bool g_InfraMode = false;
int g_dataFlowCount = 0;
bool g_DiscardJpegDec = false;
bool wcIsTx3Session = false;
bool g_UsePersistentBwControl = false;

CSIOSettings           *currentSettingsDB = nullptr;
CSIODerivedSettings    *currentDerivedSettingsDB = nullptr;

int wcJpegQuality = 0;
bool wcIsTx3TlsEnabled = false;


const char *csio_jni_getAppCacheFolder(){
    return "";
}
const char *csio_jni_getHostName(){
    return "";
}
const char *csio_jni_getDomainName(){
    return "";
}
const char *csio_jni_getServerIpAddress(){
    return "";
}
void csio_jni_SendWCMediaError( int errModule, int errCode, const char* errMessage){}
void csio_jni_SendWCServerURL( void * arg ){}
void csio_jni_SendWCBurstSettings( void * arg ){}
void csio_jni_onServerStart(){}
void csio_jni_onServerStop(){}
void csio_jni_onClientConnected(void * arg){}
void csio_jni_onClientDisconnected(void * arg){}
void csio_jni_reset_hdmi_input(){}

#include <map>
#include <string>
std::map<std::string, std::map<std::string, std::string> > gStreamConfig;

//====copy from jniMock.cpp===
static int to_android_log_level(int level)
{
	if (level == 0) return ANDROID_LOG_ERROR;
	if (level == 1) return ANDROID_LOG_WARN;
	if (level == 2) return ANDROID_LOG_INFO;
	if (level == 3) return ANDROID_LOG_DEBUG;
	return ANDROID_LOG_VERBOSE;
}

void log_impl(int level, const char *fmt, ...)
{
    // dont log anything above verbose
    if(level > 4) return;
	va_list ap;

    va_start(ap, fmt);
    __android_log_vprint(to_android_log_level(level), "TxRx.JNI", fmt, ap);
    va_end(ap);
}

static const Product_Information * prod_info = NULL;

void setup_product_info(unsigned long productId)
{
    int loopIndex = 0;
    LOGI("setup_product_info for productId: 0x%lx", productId);
	/*
	 * Iterate through the hard-coded list looking for the product name,
	 * defaulting on a product name of "" (touchpanel)
	 */
	for
	(
	  prod_info = Product_Information_Table,loopIndex=0;
	  (prod_info->product_type != (CRESTRON_PRODUCT_TYPE)0) && (prod_info->product_type != productId);
	  ++prod_info, loopIndex++
	)
	{
	    LOGI("Current loopIndex: %d", loopIndex);
        ; // Intentionally empty loop
	}

    LOGI("Current Product_Information_Table: %p", Product_Information_Table);
    LOGI("Current prod_info: %p", prod_info);
    LOGI("Current &Product_Information_Table[0]: %p", &Product_Information_Table[0]);
    LOGI("Current &Product_Information_Table[1]: %p", &Product_Information_Table[1]);
}

Product_Information * product_info (void){
    return &Product_Information_Table[0];
}