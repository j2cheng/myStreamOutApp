#ifndef _CSIO_SETTINGS_H
#define _CSIO_SETTINGS_H

#define CSIO_SETTING_6_5_2017      (2)  //added PROXY_TLS_ @6-5-2017
#define CURRENT_CSIO_SETTING_VERSION (CSIO_SETTING_6_5_2017)

#define CSIO_SETTINGS_MAGIC_NUM 0x5A5A5A5A

#define DEFAULT_PADDING_SIZE (64)
#define MAX_VIDEO_URL_SIZE      (1024)
#define MAX_ADDRESS_SIZE	(16)

#if defined AM3X00 || defined JSTR1000
#define MAX_MULTIVIDEO_WINDOWS_ALLOWED 	(10)
#else
#define MAX_MULTIVIDEO_WINDOWS_ALLOWED 	(4)
#endif

#define END_PADDING_SIZE (4096)
#define MAX_SWITCH_PORT  8

#define OSD_MESSAGE_BUFFER_MAX 1023	
#define OSD_BUF_SIZE           1024
#define OSD_TEXT 	0
#define OSD_ERR  	1
#define OSD_IMAGE	2

#define SERIAL_DEVICE_NAME_FB_MAX_LEN           63
#define MULTICASTTTL_MAX_LEN                    255
#define MULTICASTTTL_MIN_LEN                    1

#pragma pack(1)
typedef struct _MultiVideoHeader
{
    UINT16  totalLength;      //Length of all data below
    UINT8   numMessages;      //Number of messages in window list
} MultiVideoMessageHeader;
#pragma pack()

#pragma pack(1)
typedef struct _SingleVideoMessageV1
{
    UINT8   version;            //Video Details Version
    UINT8   status;             //Show Video status (1 for start, 0 for stop, 2 for property change)
    UINT16  id;                 //Video Window ID (dynamically assigned by Core3)
    UINT32  src;                //Video Source
    UINT32  left;               //Video Left Position
    UINT32  width;              //Video Width
    UINT32  height;             //Video Height
    UINT32  top;                //Video Top Position
    UINT8   digitalFB;          //Feedback Source (digital join)
    UINT16  brightness;         //Brightness
    UINT16  camType;            //Camera Type
    UINT16  camFps;             //Camera FPS
    UINT16  camCompression;     //Camera Compression
    UINT8   stretch;            //Stretch
    UINT16  urlLen;             //URL length
    INT8    url[MAX_VIDEO_URL_SIZE];                    //URL
} SingleVideoMessageV1;
#pragma pack()

#pragma pack(1)
typedef struct _MessageV2
{
	UINT8 	raic;				//Remote Annotation Input Channel (0-8)
	UINT8 	raoc;				//Remote Annotation Output Channel
	UINT16	zOrder;				//Z Order Level relative to other video windows, higher value covers lower value
	UINT8	reserved[6];		//Reserved for future use.  0 as default until defined.
} MessageV2;
#pragma pack()

#pragma pack(1)
typedef struct _SingleVideoMessageV2
{
    UINT8   version;            //Video Details Version
    UINT8   status;             //Show Video status (1 for start, 0 for stop, 2 for property change)
    UINT16  id;                 //Video Window ID (dynamically assigned by Core3)
    UINT32  src;                //Video Source
    UINT32  left;               //Video Left Position
    UINT32  width;              //Video Width
    UINT32  height;             //Video Height
    UINT32  top;                //Video Top Position
    UINT8   digitalFB;          //Feedback Source (digital join)
    UINT16  brightness;         //Brightness
    UINT16  camType;            //Camera Type
    UINT16  camFps;             //Camera FPS
    UINT16  camCompression;     //Camera Compression
    UINT8   stretch;            //Stretch
    UINT16  urlLen;             //URL length
    INT8    url[MAX_VIDEO_URL_SIZE];                    //URL
    UINT8 	raic;				//Remote Annotation Input Channel (0-8)
    UINT8 	raoc;				//Remote Annotation Output Channel
    UINT16	zOrder;				//Z Order Level relative to other video windows, higher value covers lower value
    UINT8	reserved[6];		//Reserved for future use.  0 as default until defined.
} SingleVideoMessageV2;
#pragma pack()

#pragma pack(1)
typedef struct _MultiVideoMessageWithOneVideo
{
    MultiVideoMessageHeader header;
    SingleVideoMessageV1    msg;
}MultiVideoMessageWithOneVideo;
#pragma pack()

#pragma pack(1)
typedef struct _MultiVideoMessageWithMultipleVideo
{
    MultiVideoMessageHeader header;
    SingleVideoMessageV2    msg[MAX_MULTIVIDEO_WINDOWS_ALLOWED];
}MultiVideoMessageWithMultipleVideo;
#pragma pack()

#pragma pack(1)
typedef struct _VideoMessageQueue
{
    MultiVideoMessageHeader header;
    SingleVideoMessageV2    msg[MAX_MULTIVIDEO_WINDOWS_ALLOWED];
    unsigned int 			msgIndex[MAX_MULTIVIDEO_WINDOWS_ALLOWED];
    UINT8					useSpecificIndex[MAX_MULTIVIDEO_WINDOWS_ALLOWED];
    int						specificIndex[MAX_MULTIVIDEO_WINDOWS_ALLOWED];
}VideoMessageQueue;
#pragma pack()

#pragma pack(1)
typedef struct _OSDMessageData_ {
    char            buffer[OSD_MESSAGE_BUFFER_MAX];
    unsigned char   flags;
    int             x;
    int             y;
    int             gravity;
    bool            disabled;
    uint32_t		alpha;		// PR426 0-100%
    char			color[OSD_BUF_SIZE]; // PR427 list, #ffffff or html color name
    //char			font[OSD_BUF_SIZE];  // PR428 list, hardcoded as per JP's comment
    int				size;		// PR429 should be 0-100%, currently in points FIXME
    int				decoration;	// PR430 decoration, 0x1=bold, 0x2=italic, 0x4=underline, 0x8=strikethrough
    int				shadow;		// PR431 boolean
    int				scrollRate;	// PR432 cps, unimplemented, cps makes no sense - not monospaced, needs to be in pixels
    int				scroll;		// PR433 boolean
    char			bgcolor[OSD_BUF_SIZE]; // #ffffff or html color name
    //===============================================================
    UINT8			reserved[DEFAULT_PADDING_SIZE]; //when adding new fields make sure to reduce this by # of bytes added (to end of struct!)
} OSDMessageData;
#pragma pack()

#pragma pack(1)
typedef struct _OSDImageData_ {
	int				disabled;
	char			path[OSD_BUF_SIZE];
	int				x;	// PR416 0-100%
	int				y;	// PR417 0-100%
	int             gravity;
	int				alpha;		// PR418 0-100%
	int				xScale;		// PR419 -1000% to +1000%, what does a negative % mean???
	int				yScale;		// PR420 -1000% to +1000%, what does a negative % mean???
	//===============================================================
	UINT8			reserved[DEFAULT_PADDING_SIZE]; //when adding new fields make sure to reduce this by # of bytes added (to end of struct!)
} OSDImageData;
#pragma pack()

#pragma pack(1)
typedef struct _CSIOVideoSettings
{
    UINT16  deviceMode_legacy;
    UINT16  sessionInitiationMode_legacy;
    INT32   resolution_legacy;
    INT32   frameRate_legacy;
    INT32   bitRate_legacy;
    INT32   profile_legacy;
    INT32   rtpVideoPort_legacy;
    INT32   rtpAudioPort_legacy;
    INT8    multicastAddress_legacy[MAX_VIDEO_URL_SIZE];
    INT32   tsPort_legacy;  // Tx port for NVX
    UINT16  tsEnabled_legacy;
    UINT16  streamingBuffer_legacy;
    INT16  volumeIndB_legacy;
    UINT8  rtspTCPMode_legacy;  // 0 = Auto, 1=TCP Only, 2=UDP Only
    UINT16  deprecated_debugLevel; // deprecated - was never used
    UINT8	statisticsEnabled_legacy; // Tx state for NVX
    uint16_t rtsp_port_legacy;  // tx port for nvx
    uint16_t internal_rtsp_port_legacy;
	char username_legacy [256]; // Larger than the largest serial Join
	char password_legacy [256]; // Larger than the largest serial Join
	UINT8   deprecated_videoSinkSelect; // deprecated - removed references to it elsewhere
	UINT8   hdcpActive;
	UINT16   hdcpMode_legacy;
    UINT16	streamingState_legacy;

    //===============================================================
    UINT8   rtspLastMode_legacy;
    UINT8	ignoreStopStatus_legacy;
    uint16_t rtsp_port_rx_legacy;
    uint16_t tsPort_rx_legacy;
    UINT8	statisticsEnabled_rx_legacy;
    UINT8 dscp;
    INT8    automaticInitiationMode_legacy;
    UINT8   secondaryAudioMode_legacy;
    UINT8   prevSessionInitiationMode_legacy;
	UINT8   multicastTtl_legacy;
	UINT8   fecValue_legacy;
	UINT8   frameRateDivisor;
    UINT8	reserved[DEFAULT_PADDING_SIZE-16]; //when adding new fields make sure to reduce this by # of bytes added (to end of struct!)
}CSIOVideoSettings;
#pragma pack()

//Note : Please make sure not to insert new entry in the middle
//Also adjust padding 
#pragma pack(1)
typedef struct _CSIOSettings
{
    UINT32  magicNum_legacy;
	UINT16 	versionNum_legacy;
	UINT32  padding[2];
	MultiVideoMessageWithMultipleVideo settingsMessage;
	VideoMessageQueue videoMessageQueue;
	CSIOVideoSettings videoSettings[MAX_MULTIVIDEO_WINDOWS_ALLOWED];
    OSDMessageData OSDMessage;
    OSDMessageData OSDError;
    OSDImageData OSDImage;
    INT32	maxMultiVideoIdsAllowed;
    INT32	csioLogLevel;
    UINT8   isProxyEnabled;
    UINT8   RxHdcpSupport_legacy; //0: old Receiver(none), 1 : Auto
    UINT16  playState_legacy;  //If more than >16 windows, increase this bitmap
    UINT32  donotuse_ipAddress;  //4 Bytes device IP address  //Note: depreciated since 2-20-2020 JRC
    UINT16  EgressRateLimit[MAX_SWITCH_PORT]; //Egress rate limit for fstr/jstr

    //===============================================================
    UINT32  donotuse_auxiliaryIpAddress;  //4 Bytes device IP address//Note: depreciated since 2-20-2020 JRC
    UINT8   enableIgmpSnooping;  //NVX : 1 Byte : 0 : Enable, 1 : Disabled
    UINT8   audioSource_legacy;         //NVX : 0 Automatic, 1 Input 1, 2 Input 2, 3 Analog audio, 4: Primary stream audio, 5: Secondary Stream audio
    UINT8   streamPortMode;       //NVX : 0 Automatic, 1 Manual.
    UINT8   streamPortMap;        //NVX : Port bit map for external port. Bit-mask: 0x1:port#1, 0x2:port#2 and 0x4:port#3
    UINT8   igmpVersion;          // Version number for IGMP
    UINT8   PROXY_TLS_control_legacy;     // settings to control tls mode for PROXY connection, currently used on NVX only.
    UINT8   PROXY_TLS_accessLevel_legacy; // user(client) access level used by transmitter(when in tls mode),currently used on NVX only.
    UINT8	deprecated_hdcpExternalMaxRetry;
    UINT8	dedicatedVideoSupport_legacy;
    UINT8	reserved[END_PADDING_SIZE - (13)]; //when adding new fields make sure to reduce this by # of bytes added (to end of struct!)
}CSIOSettings;
#pragma pack()

#endif //_CSIO_SETTINGS_H

// vim:ts=4:sts=4:sw=4:noet
