#ifndef __CREGSTPLAY_H__
#define __CREGSTPLAY_H__
#include "csioCommonShare.h"

/**********************************************************************
* for stream out
**********************************************************************/
/* per-stream info */
typedef struct _CRESSTREAMOUT
{
    unsigned int streamId;

    char rtsp_port[125];
    char res_x[125];
    char res_y[125];
    char frame_rate[125];
    char bitrate[125];
    char iframe_interval[125];
    char quality[125];
    char m_hdmi_in_res_x[125];
    char m_hdmi_in_res_y[125];
    bool multicast_enable;
    char multicast_address[256];
    char stream_name[256];
    char snapshot_name[256];
    bool security_enable;
    bool random_user_pw_enable;

} CRESSTREAMOUT;

/* Structure to contain all our information, so we can pass it to callbacks */
typedef struct _CustomStreamOutData
{
    // jobject app;            /* Application instance, used to call its methods. A global reference is kept. */

    //void * surface;           /* not sure yet */

    CRESSTREAMOUT streamOut[MAX_STREAMS];
} CustomStreamOutData;

#define DEFAULT_UDP_BUFFER 20971520 //in bytes (20 Mb),we might need to increase it for 4K streaming

#endif