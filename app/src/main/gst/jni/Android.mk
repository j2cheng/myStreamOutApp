LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

ifndef GSTREAMER_SYSROOT
$(error GSTREAMER_SYSROOT is not defined! Source platforms/android/env.sh or use build_android.sh)
endif

GSTREAMER_ROOT           := $(GSTREAMER_SYSROOT)
GSTREAMER_NDK_BUILD_PATH := $(GSTREAMER_SYSROOT)/share/gst-android/ndk-build/

# Plugins statically linked into libgstreamer_android.so.
# Chosen to cover what streamout.cpp / camera2_gst_streamer.cpp actually use:
#   appsrc/appsink, h264parse/h265parse, aacparse, rtph26xpay, rtpmp4apay,
#   rtpbin, opusenc, amc* (MediaCodec), opensles, rtsp, sdp, etc.
GSTREAMER_PLUGINS := \
    androidmedia \
    app \
    audioconvert \
    audioparsers \
    audioresample \
    audiotestsrc \
    coreelements \
    coretracers \
    isomp4 \
    jpeg \
    jpegformat \
    mpegtsdemux \
    mpegtsmux \
    opengl \
    opensles \
    opus \
    rawparse \
    rtp \
    rtpmanager \
    rtsp \
    sdpelem \
    tcp \
    typefindfunctions \
    udp \
    videoconvertscale \
    videocrop \
    videofilter \
    videoparsersbad \
    videorate \
    videotestsrc \
    x264

GSTREAMER_EXTRA_DEPS := \
    glib-2.0 \
    gstreamer-app-1.0 \
    gstreamer-audio-1.0 \
    gstreamer-net-1.0 \
    gstreamer-rtsp-1.0 \
    gstreamer-rtsp-server-1.0 \
    gstreamer-sdp-1.0 \
    gstreamer-video-1.0 \
    libcrypto \
    libssl \
    openssl

include $(GSTREAMER_NDK_BUILD_PATH)/gstreamer-1.0.mk
