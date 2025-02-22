LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

ifeq ($(TARGET_HAS_VPP),true)
LOCAL_CFLAGS += -DTARGET_HAS_VPP
endif

LOCAL_SRC_FILES := \
    VideoDecoderHost.cpp \
    VideoDecoderBase.cpp \
    VideoDecoderWMV.cpp \
    VideoDecoderMPEG4.cpp \
    VideoDecoderAVC.cpp \
    VideoDecoderTrace.cpp

# LOCAL_CFLAGS :=

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH) \
    $(TARGET_OUT_HEADERS)/libva \
    $(TARGET_OUT_HEADERS)/libmixvbp

ifeq ($(USE_INTEL_SECURE_AVC),true)
LOCAL_CFLAGS += -DUSE_INTEL_SECURE_AVC
LOCAL_SRC_FILES += securevideo/$(TARGET_BOARD_PLATFORM)/VideoDecoderAVCSecure.cpp
LOCAL_C_INCLUDES += $(LOCAL_PATH)/securevideo/$(TARGET_BOARD_PLATFORM)
LOCAL_CFLAGS += -DUSE_INTEL_SECURE_AVC
endif

PLATFORM_USE_GEN_HW := \
    baytrail \
    cherrytrail
ifneq ($(filter $(TARGET_BOARD_PLATFORM),$(PLATFORM_USE_GEN_HW)),)
    LOCAL_CFLAGS += -DUSE_AVC_SHORT_FORMAT -DUSE_GEN_HW
endif


PLATFORM_USE_HYBRID_DRIVER := \
    baytrail

ifneq ($(filter $(TARGET_BOARD_PLATFORM),$(PLATFORM_USE_HYBRID_DRIVER)),)
    LOCAL_CFLAGS += -DUSE_HYBRID_DRIVER
endif

PLATFORM_SUPPORT_SLICE_HEADER_PARSER := \
    merrifield \
    moorefield

ifneq ($(filter $(TARGET_BOARD_PLATFORM),$(PLATFORM_SUPPORT_SLICE_HEADER_PARSER)),)
    LOCAL_CFLAGS += -DUSE_SLICE_HEADER_PARSING
endif

#LOCAL_LDLIBS += -lpthread

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libmixvbp \
    libva \
    libva-android \
    libva-tpi


#LOCAL_CFLAGS += -DANDROID

#LOCAL_SHARED_LIBRARIES += liblog

LOCAL_COPY_HEADERS_TO  := libmix_videodecoder

LOCAL_COPY_HEADERS := \
    VideoDecoderHost.h \
    VideoDecoderInterface.h \
    VideoDecoderDefs.h

ifneq ($(filter $(TARGET_BOARD_PLATFORM),$(PLATFORM_SUPPORT_SLICE_HEADER_PARSER)),)
    LOCAL_COPY_HEADERS += securevideo/$(TARGET_BOARD_PLATFORM)/VideoFrameInfo.h
endif

LOCAL_MODULE_TAGS := optional
LOCAL_MODULE := libva_videodecoder

ifeq ($(USE_HW_VP8),true)
LOCAL_SRC_FILES += VideoDecoderVP8.cpp
LOCAL_CFLAGS += -DUSE_HW_VP8
endif

ifeq ($(USE_SW_MPEG4),true)
LOCAL_CFLAGS += -DUSE_SW_MPEG4
endif

include $(BUILD_SHARED_LIBRARY)
