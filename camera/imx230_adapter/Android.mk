LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := libimx230_op2_adapter
LOCAL_SRC_FILES := imx230_adapter.c
LOCAL_SHARED_LIBRARIES := libdl liblog
LOCAL_CFLAGS := -Wall -Werror
LOCAL_MULTILIB := 32
LOCAL_VENDOR_MODULE := true
include $(BUILD_SHARED_LIBRARY)
