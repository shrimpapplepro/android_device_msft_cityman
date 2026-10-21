LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
# IMX230 OP2 adapter. Loaded by the OP2 daemon under the Moto sensor name;
# the original Moto lib is installed as libmmcamera_mot_imx230_moto.so.
LOCAL_MODULE := libmmcamera_mot_imx230
LOCAL_SRC_FILES := imx230_adapter.c
LOCAL_SHARED_LIBRARIES := libdl liblog
LOCAL_CFLAGS := -Wall -Werror
LOCAL_MULTILIB := 32
LOCAL_VENDOR_MODULE := true
include $(BUILD_SHARED_LIBRARY)
