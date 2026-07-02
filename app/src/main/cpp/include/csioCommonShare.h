#ifndef _CSIO_COMMON_SHARE_H
#define _CSIO_COMMON_SHARE_H

#define MAX_STREAMS 4
// #define MAX_MULTIVIDEO_WINDOWS_ALLOWED 1
#define UINT32 uint32_t
#define UINT16 uint16_t
#define UINT8 uint8_t
#define INT8 int8_t
#define INT16 int16_t
#define INT32 int32_t
#include <android/log.h>
#include <string.h>
#include <pthread.h>

// #include "productName.h"
// #include "CSIOProductSpecific.h"
#include "csioSettings.h"

#include <string>
#include <list>

#ifndef THERESERIOUSLYISNTAMACROFORTHISYET
#define THERESERIOUSLYISNTAMACROFORTHISYET(x) #x
#endif

#ifndef STRINGIFY
#define STRINGIFY(x) THERESERIOUSLYISNTAMACROFORTHISYET(x)
#endif

// #ifndef OVERRIDE_CSIO_LOG
// // If PRINT_TO_SYSTEM_LEVEL_ERROR_LOG bit is enabled print to error server and syslog
// // If log level is above debug than append file and line number to string formatter
// #define  CSIO_LOG(LOGLEVEL, ...)  \
// 	(LOGLEVEL & PRINT_TO_SYSTEM_LEVEL_ERROR_LOG) ? \
// 	({csio_log_to_error_server((eLogLevel)(LOGLEVEL & 0xFF), __VA_ARGS__); CSIO_LOG_WRAPPER((eLogLevel)(LOGLEVEL & 0xFF), __VA_ARGS__);}) :\
// 	CSIO_LOG_WRAPPER((eLogLevel)(LOGLEVEL & 0xFF), __VA_ARGS__)

// #define CSIO_LOG_WRAPPER(LOGLEVEL, ...) \
// 	(currentSettingsDB->csioLogLevel < eLogLevel_verbose) ? csio_log(LOGLEVEL, __VA_ARGS__) :\
// 	csio_log(LOGLEVEL, "[" __FILE__ ":" STRINGIFY(__LINE__) "] " __VA_ARGS__)
// #else // OVERRIDE_CSIO_LOG
// #include "log_impl.h"
// #endif // OVERRIDE_CSIO_LOG
//#define CSIO_LOG(level, fmt, ...) printf(fmt, ##__VA_ARGS__)
// #define CSIO_LOG(level, fmt, ...) __android_log_print(ANDROID_LOG_ERROR, "streamout", fmt, ##__VA_ARGS__)


#define CSIO_LOG(level, fmt, ...) log_impl(level, fmt, ##__VA_ARGS__)
#define STRLOG(level, category, fmt, ...)  log_impl(level, fmt, ##__VA_ARGS__)

void log_impl(int level, const char *fmt, ...);



#define CSIOSettings_file 		("/data/CresStreamSvc/CSIOSettings")
#define CSIOSettings_ramfile 	("/dev/shm/crestron/CresStreamSvc/CSIOSettings")
#define DEDICATED_VIDEO_FLAG_FILE_PATH			"/data/CresStreamSvc/dedicatedVideoFlag"

#define ERRORSTATUSMSG_MAX    1024

#define AIRMEDIA_WCCONFIGURATION_PERIPHERALVOLUME_MIN 0
#define AIRMEDIA_WCCONFIGURATION_PERIPHERALVOLUME_MAX 100


// because of PRINT_TO_SYSTEM_LEVEL_ERROR_LOG define enum should not go higher than 255
typedef enum
{
	eLogLevel_error = 0,
	eLogLevel_warning,
	eLogLevel_info,
	eLogLevel_debug,
	eLogLevel_verbose,
	eLogLevel_extraVerbose,

	// Do not add anything after the Last state
	eLogLevel_LAST
}eLogLevel;

// tags for poor men's implementation of log categories
// #define TAG_SECAUDIO
#define TAG_SECAUDIO       "secaudio: "
#define TAG_INDEPROUTING   "indeproute: "
#define TAG_ADAPTBITRATE   "adaptbitrate: "
#define TAG_AUTOINIT       "autoinit: "
#define TAG_STREAMAUTH     "strauth: "


// needs to be below the above typedef
//#include "loggingControl.h"


typedef enum _eCsioStatus
{
   CSIO_SUCCESS,
   CSIO_FAILURE,
   CSIO_CANNOT_START_PIPELINE,
   CSIO_STREAM_NOT_FOUND,
   CSIO_CANNOT_CREATE_ELEMENTS,
   CSIO_MISSING_LOCATION,
   CSIO_COULD_NOT_SET_LOCATION,
   CSIO_COULD_NOT_START_PIPELINE,
   CSIO_UNSUPPORTED_STREAM_MPEG4,
   CSIO_INVALID_INPUT_ARGS,
   CSIO_CANNOT_INSTALL_HDLR,
   CSIO_GSTREAMER_ERROR,
   CSIO_GSTREAMER_WARNING,
   CSIO_SRC_NOT_REACHABLE,
   CSIO_INVALID_URI,
   CSIO_INSTANCE_ALREADY_RUNNING,
   CSIO_CANNOT_CLEAR_OVERLAY,
   CSIO_CANNOT_LINK_ELEMENTS,
   CSIO_PIPELINE_NOT_DEFINED,
   CSIO_RECEIVE_ERROR,
   CSIO_CANNOT_ACCESS_STREAM,
   CSIO_CANNOT_LINK_STREAM,
   CSIO_AUDIO_BLOCKED,
   CSIO_STATUS_MAX
} eCsioStatus;

//Note: TLS_mode that cames from csio DB settings
typedef enum _ePROXY_TLS_MODE
{
    PROXY_TLS_MODE_NONE = 0,               //everything is off
    PROXY_TLS_MODE_TLS_ON,                 //only enable TLS(default)
    PROXY_TLS_MODE_TLS_SERV_CA_AUTH,       //enable TLS plus server doing client CA authentication
    PROXY_TLS_MODE_TLS_CLIENT_VALIDATE,    //enable TLS plus server validates client with usr/passwd
    PROXY_TLS_MODE_TLS_SERV_CA_AUTH_CLIENT_VALIDATE, //enable TLS plus server validates client with usr/passwd,plus client doing server CA authentication

    PROXY_TLS_MODE_MAX
}ePROXY_TLS_MODE;

#define CSIO_DEFAULT_LOG_LEVEL (eLogLevel_debug)
#define PRINT_TO_SYSTEM_LEVEL_ERROR_LOG (0x100)
#define CSIO_LOG_LEVEL_SAVE_FILE ("/data/CresStreamSvc/csioLogLevel")

#define CSIO_DEFAULT_PROXY_TLS_CONTROL      (PROXY_TLS_MODE_NONE)
#define CSIO_DEFAULT_PROXY_TLS_ACCESS_LEVEL (1)   // 1 - CONNECTION:  is defined in ADService.h typedef enum AccessLevel

// Enumeration to indicate the state of the stream.
// !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
// IMPORTANT: Update status text in the array in csioCommonShare.cpp
// !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
typedef enum _eStreamState
{
	// Basic States
	STREAMSTATE_STOPPED=0,
	STREAMSTATE_STARTED,
	STREAMSTATE_PAUSED,

	// Add additional states after this
	STREAMSTATE_CONNECTING,
	STREAMSTATE_RETRYING,
	STREAMSTATE_CONNECTREFUSED,
	STREAMSTATE_BUFFERING,
	STREAMSTATE_CONFIDENCEMODE,
	STREAMSTATE_STREAMERREADY,
	STREAMSTATE_HDCPREFUSED,
	STREAMSTATE_INVALIDPARAMETER,//Note: should trigger DIGITAL_PROCESSING_FB go low
	STREAMSTATE_INVALIDCODEC,

   STREAMSTATE_AUTHFAILURE,

	// Do not add anything after the Last state
	STREAMSTATE_LAST
} eStreamState;

typedef enum _eInvalidStartErrors
{
    INVALIDSTART_IP_ADDRESS = -1,
	INVALIDSTART_MAC_ADDRESS = -2,
	INVALIDSTART_MULTICAST_ADDRESS = -3,
	INVALIDSTART_URL = -4,
} _eInvalidStartErrors;

typedef enum _eRxHdcpState
{
    HDCPSTATE_NOT_CONNECTED = 0,
    HDCPSTATE_ENCRYPTED,
    HDCPSTATE_UNENCRYPTED,
    HDCPSTATE_AUTH_REJECT_BY_TX,
    HDCPSTATE_MAX_RECEIVERS_REACH,
    HDCPSTATE_LAST

} eRxHdcpState;

// ***********************************************************************************
// Keep updated with CresStreamSvc in CresStreamCtrl.java!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
// ***********************************************************************************
typedef enum _eHardwarePlatform
{
    eHardwarePlatform_iMx53,
    eHardwarePlatform_iMx6,
    eHardwarePlatform_OMAP5,
    eHardwarePlatform_Arria10,
    eHardwarePlatform_Amlogic,
    eHardwarePlatform_Snapdragon,
    eHardwarePlatform_Rockchip,
    eHardwarePlatform_Snapdragon_TST1080,
    eHardwarePlatform_Qualcomm,
    eHardwarePlatform_Qualcomm_TSX80,
} eHardwarePlatform;

typedef enum _ePauseCapability
{
    ePauseCapability_NoPauseSupport,
	ePauseCapability_PausePipeline,
	ePauseCapability_PauseWithValves,
	ePauseCapability_PauseLast,
} ePauseCapability;

// The following values match values found in Crestron database file PanelDef2000.mdb, "Video Input Source List" table.
// This file is typically installed on a PC to C:\Program Files (x86)\Crestron.
// You can view the values by exporting the database table to a comma-delimited file.  In Linux:
//   sudo apt-get install mdbtools
//   mdb-export PanelDef2000.mdb "Video Input Source List" > "Video Input Source List.csv"

typedef enum
{
	VIDEO_SOURCE_H264 				= 11,	// Legacy
	VIDEO_SOURCE_MJPEG				= 12,	// Legacy
	VIDEO_SOURCE_HDMI_IN_PREVIEW 	= 20,
	VIDEO_SOURCE_CAMERA_PREVIEW 	= 21,
	VIDEO_SOURCE_AIRMEDIA 			= 42,
	VIDEO_SOURCE_WBS_VIDEO          = 63,   // Temp placeholder until real value is determined
	VIDEO_SOURCE_STREAMING_VIDEO 	= 252,
	VIDEO_SOURCE_MJPEG_VIEWER 		= 253,
	VIDEO_SOURCE_DM_IN 				= 99
} VTPROVIDEOSOURCE;

typedef enum
{
    RES_LIMIT_H264_DECODER,
    RES_LIMIT_MJPEG_DECODER,
    RES_LIMIT_H264_ENCODER,
    RES_LIMIT_H265_DECODER,
    MAX_VIDEO_CODECS
} MAXCODECRESOLUTION;

#define MAX_SLOT_CAPABILITY_SIZE (15)
#define MAX_VIDEO_CODEC_STRING_SIZE (256)

#define SMART_GRAPHICS_BITMASK_RUNS_SG     (0x01)
#define SMART_GRAPHICS_BITMASK_RUNS_SPLASH (0x02)
#define SMART_GRAPHICS_BITMASK_RUNS_BB     (0x04)
#define SMART_GRAPHICS_BITMASK_RUNS_PPUX   (0x08)

//Indicates products that support only alphablending
#define PRODUCT_BITMASK_ONLY_ALPHABLEND    (0x10)

#define GSTREAMER_BITMASK_NATIVE_GSTREAMER (0x0001)
#define GSTREAMER_BITMASK_GSTREAMER_0_1 (0x0002)
#define GSTREAMER_BITMASK_GSTREAMER_1_0 (0x0004)
#define GSTREAMER_BITMASK_GSTREAMER_1_2 (0x0008)
#define GSTREAMER_BITMASK_GSTREAMER_1_4 (0x0010)
#define GSTREAMER_BITMASK_GSTREAMER_1_8 (0x0020)
#define GSTREAMER_BITMASK_RETRIEVE_AND_SAVE_SDP (0x0040)

#define STATISTICS_BITMASK_ENABLED (0x01)
#define STATISTICS_BITMASK_ALWAYS_ON (0x03)

// used for join response filtering table
#define JOIN_RESPONSE_NO_SLOTS (0x200)
#define JOIN_RESPONSE_ALL_SLOTS (0x300)

// Even though slot numbers are one byte, use UINT32.  This way,
// we can have custom numbers that don't collide with the slot
// number namespace.  This will not take up much more memory.
typedef struct _Slot_Conversion_Unit
{
	UINT32	platformNum;
	UINT32	csioNum;
}Slot_Conversion_Unit;

typedef struct _Codec_Resolution
{
    int  width;
    int  height;
}Codec_Resolution;
typedef struct _Video_Codec_Config
{
    Codec_Resolution max;
    int minPercentFS; //minimum stream resolution allowed as a %display fullscale
}Video_Codec_Config;

//cpy from in productDefs.h
#define SYSTEM_TSX80  		0x6F00      // For x80 product family 


typedef struct _Product_Information
{
    UINT32                	product_type;
    uint16_t 				iplink_reserve_join_number;
    uint16_t 				iplink_tcp_port;
    eHardwarePlatform 		hw_platform;
    // Bit 0 = Runs SmartGraphics, Bit 1 = Runs Splash, Bits 2-7 = Reserved
    UINT8 					smart_graphics_bitmask;
    //Bit 0 = native Gstreamer, Bit 1 = Gstreamer 0.1, Bit 2 = Gstreamer 1.0, Bit 3 = Gstreamer 1.2, Bit 4 = Gstreamer 1.4, Bit 5 = Gstreamer 1.6, Bit 6-15 = Reserved
    UINT16 					gstreamer_bitmask;
    //Bit 0 = statistics enabled, Bit 1 = always capture statistics, Bits 2-7 = Reserved
    UINT8 					stream_statistics_bitmask;
	//Bit 0 = can display streams on primary display
	//Bit 1 = can display streams on external display
	//Bit 2 = can display streams on a 3rd display
	//and so on...
	UINT32  				stream_display_bitmask;
	//Bit 0 = primary display needs HDCP check
	//Bit 1 = external display needs HDCP check
	//Bit 2 = 3rd display needs HDCP check
	UINT32					display_check_hdcp_bitmask;
    ePauseCapability		pause_capability;
    bool					stop_on_messagepage;
    UINT8					maximum_video_windows;
    UINT8					maximum_local_video_windows;	// Maximum number of video windows running on local processor
    UINT8					maximum_stream_id;				// Maximum audio/video stream id (0-based)
    INT8                    maximum_txrx_id;                // Maximum TXRX id (0-based, -1 for disable)
    UINT32					join_response_slots_digital[MAX_SLOT_CAPABILITY_SIZE];
    UINT32					join_response_slots_analog[MAX_SLOT_CAPABILITY_SIZE];
    UINT32					join_response_slots_serial[MAX_SLOT_CAPABILITY_SIZE];
    bool					should_translate_slots;
    Slot_Conversion_Unit    slot_translation_map[MAX_SLOT_CAPABILITY_SIZE];
	UINT32                  dm_input_cnt;	// How many dm inputs the product has, if any
	bool					process_hdmi_in_audio;	// whether or not to take audio from hdmi in and forward via software to hdmi out
	char					H264_decoder_string[MAX_VIDEO_CODEC_STRING_SIZE]; //this is the string defined in Media_Codecs.xml
	char 					mjpeg_decoder_string[MAX_VIDEO_CODEC_STRING_SIZE]; //this is the string defined in Media_Codecs.xml
	char					H264_encoder_string[MAX_VIDEO_CODEC_STRING_SIZE]; //this is the string defined in Media_Codecs.xml
	char					H265_decoder_string[MAX_VIDEO_CODEC_STRING_SIZE]; //this is the string defined in Media_Codecs.xml
	bool                    proxy_support;
	UINT32					dcp_mode;	// DCP mode supported on this device
	UINT32					output_audio_samplerate; //What the output audio sample rate is (i.e. 480000 = 48 kHz)
	UINT32                  chroma_key_used; //product uses chroma key or not
	bool					hide_video_on_stop;	// whether product should attempt to hide video surface before actually stopping video
	bool                    hasAutomaticInitiationMode; // whether product has automatic initiation mode
	bool                    doesProductHaveDsp; // whether product has a DSP (NVX only right now)
	bool					doesProductHaveExternalHDMI;	// whether product has external HDMI (HDCP status comes from external device)
	UINT8					numberEthernetAdapters;			// The total number of ethernet adapters
	UINT8					primaryEthernetAdapter;			// Which adapter index is considered the primary adatper
	bool					mapSettingsToSdcard;
	bool					restartStreamsOnStart;			// Whether to auto restart streams on reboot/powercycle
	bool                    doesProductHaveMiracast;        // whether miracast is suppoerted
	Video_Codec_Config      videoCodecCfg[MAX_VIDEO_CODECS];   //table of video CODEC-specific parameters
	bool                    airMedia_enabled;               // true if AirMedia is enabled by default
    bool                    productHasHDMIoutput;           // whether or not the product has an HDMI output port
    bool                    does4kAirMediaDecode;           // whether or not the product supports 4k video airMedia decode
}Product_Information;

typedef struct _CSIODerivedSettings
{
    int isUseProxy[MAX_MULTIVIDEO_WINDOWS_ALLOWED];

}CSIODerivedSettings;


#define INTERNAL_RTSP_PORT 5540

#define HARDCODED_FOR_NOW_RTSPS_PORT    322


typedef enum
{
   ePROTOCOL_RTSP_TCP,
   ePROTOCOL_RTSP_UDP,
   ePROTOCOL_RTSP_UDP_TS,
   ePROTOCOL_RTSP_TS,
   ePROTOCOL_HTTP,
   ePROTOCOL_UDP_TS,
   ePROTOCOL_UDP,
   ePROTOCOL_UDP_BPT,
   ePROTOCOL_MULTICAST,
   ePROTOCOL_MULTICAST_TS,
   ePROTOCOL_FILE,
   ePROTOCOL_DEFAULT_RTSP_TCP,
   ePROTOCOL_TCPSERVER_RCV,//create tcp server socket to receive video
   ePROTOCOL_MAX,
} eProtocolId;

typedef enum
{
    eHttpMode_UNSPECIFIED = 0,    //default value
    eHttpMode_MJPEG,
    eHttpMode_MP4,
    eHttpMode_HLS,
    eHttpMode_DASH,
    eHttpMode_MSS
} eHttpMode;

typedef enum
{
   eWindowId_0,
   eWindowId_1,
   eWindowId_2,
   eWindowId_3,
}eWindId;

typedef enum
{
   c_RTPVideoPortNumber,
   c_RTPAudioPortNumber,
   c_TSportNumber,
}ePortId;

typedef enum
{
    AIRMEDIA_LOGINCODEMODE_DISABLED,
    AIRMEDIA_LOGINCODEMODE_RANDOM,	// 4-digit login code
    AIRMEDIA_LOGINCODEMODE_FIXED,
    AIRMEDIA_LOGINCODEMODE_RANDOM8,	// 8-digit login code
    AIRMEDIA_LOGINCODEMODE_MAX
} eAirMediaLoginCodeMode;

typedef enum
{
    AIRMEDIA_CANVASOPTIONS_ALLSOURCES,
    AIRMEDIA_CANVASOPTIONS_NETWORKSOURCES,
    AIRMEDIA_CANVASOPTIONS_MAX
} eAirMediaCanvasOptions;

typedef enum
{
    AIRMEDIA_WCCONFIGURATIONQUALITYMODE_HIGH,
    AIRMEDIA_WCCONFIGURATIONQUALITYMODE_NORMAL,
    AIRMEDIA_WCCONFIGURATIONQUALITYMODE_LOW,
    AIRMEDIA_WCCONFIGURATIONQUALITYMODE_MAX
} eAirMediaWCConfigurationQualityMode;

typedef enum
{
    AIRMEDIA_WCSTATUSPERIPHERALBLOCKEDREASON_NOTREADY,
    AIRMEDIA_WCSTATUSPERIPHERALBLOCKEDREASON_CONFIGURING,
    AIRMEDIA_WCSTATUSPERIPHERALBLOCKEDREASON_INSTANDBY,
    AIRMEDIA_WCSTATUSPERIPHERALBLOCKEDREASON_NOHDMIOUTSYNC,
    AIRMEDIA_WCSTATUSPERIPHERALBLOCKEDREASON_MAX
} eAirMediaWCStatusPeripheralBlockedReason;

typedef enum
{
    RX_HDCP_SUPPORT_AUTO,
    RX_HDCP_SUPPORT_OFF,
    RX_HDCP_SUPPORT_MAX
} eRxHdcpSupport;

typedef enum
{
    TX_HDCP_SUPPORT_FOLLOW_INPUT,
    TX_HDCP_SUPPORT_ALWAYS,
    TX_HDCP_SUPPORT_NEVER,
    TX_HDCP_SUPPORT_MAX
} eTxHdcpSupport;

#ifdef __cplusplus
//Any modifications made to this class must also be implemented in corresponding java class CresLog
namespace CCresLogCode
{
    typedef enum
    {
        Error_None = 0,
        Error_HDMI_No_Sync = 1,
        Error_DM_No_Stream = 2,
        Error_Connection_Refused = -1,
        Error_No_Network = -2,
        Error_Generic_Retry = -1000,
        Error_Invalid_Credentials = -1001,
        Error_Invalid_Hostname = -1002,
        Error_Unsupported_Codec = -1003,
        Error_Generic_No_Retry = -9000,
        Error_Unsupported_Source_Type = -9001,
        Error_Invalid_Url = -9002,
        Error_Exceeded_Maximum_Source_Type_Sessions = -9003,
        Error_Exceeded_Maximum_Total_Sessions = -9004,
        Error_Invalid_Surface = -9005,
        Error_Invalid_Stop_Command = -9006,
        Error_Non_HDCP_Monitor     = -9008//skip 9007 here
    } eStreamError;
};
#endif

extern CSIOSettings    *currentSettingsDB;
extern CSIODerivedSettings    *currentDerivedSettingsDB;

extern const char * const StreamStateText[];
extern const char * const RxHdcpStateText[];

typedef enum
{
	ePlayStatus = 0,
	ePauseStatus
}eSaveFileTypes;

typedef enum
{
    eVlanIntfMediaVideo = 0,
    eVlanIntfMediaAudio = 1,

}eVlanInterfaceMediaIndex;

//Note: this is used by
//      send_current_ethernet_interface_information()
//      to trigger restart stream du to ip changed or
//      link status changed
typedef enum
{
    eIpChanged         = 1,
    eLinkStatusChanged = 2,
    eVlanIpChanged         = 4,   //for ipv4
    eVlanLinkStatusChanged = 8,
    eIntfNameChanged       = 16,
    eIntfDeleted           = 32,
    eVlanIpv6Changed       = 64,  //for ipv6
}eIpOrLinkStatusChanged;

//Note: this is used by
//      send_current_ethernet_interface_information()
typedef struct{
    std::string name;
    int ip_link_changes;
    int updatReq;
}ethernetIntfInfoType;

void saveStateToDisk(eSaveFileTypes fileType, int (*stateArray)[MAX_MULTIVIDEO_WINDOWS_ALLOWED]); //TODO: we should eventually serdes all of CSIOSettings
void retreiveStateFromDisk(eSaveFileTypes fileType, int (*stateArray)[MAX_MULTIVIDEO_WINDOWS_ALLOWED]);
std::string STR2CSTR(const char *stringFormat, ...);
void csio_sendErrorStatusMessage(int errorCode, std::string diagnosticMessage, int streamId=-1, int sendto=1); //sendto: 1=SENDTOCRESSTORE_PUBLISH

// Bitmask for DCP support on device
#define DCP_MODE_NONE       (0x0)
#define DCP_MODE_TXRX       (0x1)

// This header file is included by jni.c but also by .cpp files,
// that's why we need the ifdef __cplusplus here.
#ifdef __cplusplus
extern "C"
{
#endif
void csio_setup_product_info(int);
int GetInPausedState(unsigned stream_id);
void SetInPausedState(unsigned stream_id, unsigned paused);
Product_Information * product_info (void);
void csio_log(eLogLevel logLevel, const char *stringFormat, ...);

void csio_logalways(eLogLevel logLevel, const char * categoryStr, const char * stringFormat, ...) __attribute__ ((format (printf, 3, 4)));

void csio_log_to_error_server(eLogLevel logLevel, const char *stringFormat, ...) __attribute__ ((format (printf, 2, 3)));
void csio_log_save_log_level(eLogLevel logLevel);
int csio_log_retreive_log_level();
void csio_ForceQuit();

void csio_createMultiControlMtx();
void csio_deleteMultiControlMtx();
void csio_getMultiControlMtx();
void csio_releaseMultiControlMtx();
int  csio_GetStreamingStatesCnt();

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
class Mutex
{
public:
    Mutex()
    {
        pthread_mutex_init(&m_lock, NULL);
        is_locked = false;
    }

    ~Mutex()
    {
        if( is_locked )
            unlock();

        pthread_mutex_destroy(&m_lock);
    }

    void lock()
    {
        pthread_mutex_lock(&m_lock);
        is_locked = true;
    }

    void unlock()
    {
        is_locked = false; //do it BEFORE unlocking to avoid race condition
        pthread_mutex_unlock(&m_lock);
    }

    pthread_mutex_t *get_mutex_ptr()
    {
        return &m_lock;
    }

//private:
    pthread_mutex_t m_lock;
    volatile bool is_locked;
};

class CondVar
{
public:
    CondVar()
    {
#if defined(ANDROID_OS) && !defined(AM3X00)
        pthread_cond_init(&m_cond_var, NULL);
#else
        pthread_condattr_t condattr;
        pthread_condattr_init(&condattr);
        pthread_condattr_setclock(&condattr,CLOCK_MONOTONIC);

        pthread_cond_init(&m_cond_var, &condattr);
        pthread_condattr_destroy(&condattr);
#endif
    }

    ~CondVar()
    {
        pthread_cond_destroy(&m_cond_var);
    }

    int wait(pthread_mutex_t *mutex)
    {
        int rc = pthread_cond_wait(&m_cond_var, mutex);
        return(rc);
    }

    void signal()
    {
        pthread_cond_signal(&m_cond_var);
    }

    void broadcast()
    {
        pthread_cond_broadcast(&m_cond_var);
    }

    int waittimedcont(pthread_mutex_t *mutex, struct timespec *ts)
    {
#if defined(HAVE_PTHREAD_COND_TIMEDWAIT_MONOTONIC) || defined(AM3X00)
        int rc = pthread_cond_timedwait_monotonic_np(&m_cond_var, mutex, ts);
#else
        int rc = pthread_cond_timedwait(&m_cond_var, mutex, ts );
#endif
        return(rc);
    }

    pthread_cond_t *get_cond_ptr()
    {
        return &m_cond_var;
    }

private:
    pthread_cond_t m_cond_var;
};

#define MW_MAXENTRIES                  4
#define MW_MAXIDSTRSIZE                16

#define MW_TEST_FREEZEUPDATES          0x01
#define MW_TEST_INHIBITCALLER          0x02

typedef struct _multiWatchdogEntry
{
   char        uniqueIDStr[MW_MAXIDSTRSIZE];
   struct timeval lastTimeStamp;
   int         maxUpdateInterval;            // in seconds
   unsigned int testCtrl;                    // for testing only!
} MULTIWATCHDOGENTRY;

typedef struct _multiWatchdog
{
   int         entriesCnt;
   pthread_mutex_t  mwMutex;
   pthread_t   mwThread;
   MULTIWATCHDOGENTRY mwEntries[MW_MAXENTRIES];
} MULTIWATCHDOG;

int mwInit();
int mwAddWatchdog(char * uniqueIDStr, int maxUpdateInterval);
int mwRemoveWatchdog(char * uniqueIDStr);
int mwRefreshWatchdog(char * uniqueIDStr);
int mwSetWatchdogCtrl(char * uniqueIDStr,unsigned int ctrlFlags);
int mwGetWatchdogCtrl(char * uniqueIDStr);
void * MallocProbingThread(void * theArg);
#endif // cplusplus


#endif //_CSIO_COMMON_SHARE_H
