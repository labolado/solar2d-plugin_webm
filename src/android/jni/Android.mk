LOCAL_PATH := $(call my-dir)

CORONA_NATIVE := /Applications/CoronaEnterprise
CORONA_ROOT := $(CORONA_NATIVE)/Corona
LUA_API_DIR := $(CORONA_ROOT)/shared/include/lua
LUA_API_CORONA := $(CORONA_ROOT)/shared/include/Corona

SRC_DIR := ../../shared
THIRD_PARTY := $(SRC_DIR)/../../third_party

HEADER_DIR := $(SRC_DIR)/include
# WebM decode stack headers
LIBVPX_API_DIR  := $(THIRD_PARTY)/libvpx
LIBWEBM_API_DIR := $(THIRD_PARTY)/libwebm
OPUS_API_DIR    := $(THIRD_PARTY)/opus/include

######################################################################

include $(CLEAR_VARS)
LOCAL_MODULE := liblua
LOCAL_SRC_FILES := ../corona-libs/jni/$(TARGET_ARCH_ABI)/liblua.so
LOCAL_EXPORT_C_INCLUDES := $(LUA_API_DIR)
include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := libcorona
LOCAL_SRC_FILES := ../corona-libs/jni/$(TARGET_ARCH_ABI)/libcorona.so
LOCAL_EXPORT_C_INCLUDES := $(LUA_API_CORONA)
include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := libopenal
LOCAL_SRC_FILES := ../corona-libs/jni/$(TARGET_ARCH_ABI)/libopenal.so
include $(PREBUILT_SHARED_LIBRARY)

######################################################################
# WebM decode stack (built by build_third_party_libs.sh into jni/<abi>/).

include $(CLEAR_VARS)
LOCAL_MODULE := libvpx
LOCAL_SRC_FILES := $(TARGET_ARCH_ABI)/libvpx.a
LOCAL_EXPORT_C_INCLUDES := $(LIBVPX_API_DIR)
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := libwebm
LOCAL_SRC_FILES := $(TARGET_ARCH_ABI)/libwebm.a
LOCAL_EXPORT_C_INCLUDES := $(LIBWEBM_API_DIR)
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := libopus
LOCAL_SRC_FILES := $(TARGET_ARCH_ABI)/libopus.a
LOCAL_EXPORT_C_INCLUDES := $(OPUS_API_DIR)
include $(PREBUILT_STATIC_LIBRARY)

######################################################################

include $(CLEAR_VARS)

LOCAL_MODULE := libplugin.webm

LOCAL_C_INCLUDES := $(LOCAL_PATH)/$(HEADER_DIR)

LOCAL_SRC_FILES := $(SRC_DIR)/src/lua/WebMTextureBinding.cpp \
    $(SRC_DIR)/src/decoders/WebMDecoder.cpp \
    $(SRC_DIR)/generated/plugin_webm.c

LOCAL_CFLAGS := \
    -DANDROID_NDK \
    -DNDEBUG \
    -D_REENTRANT \
    -DRtt_ANDROID_ENV \
    -O3 \
    -fPIC \
    -DPIC

LOCAL_CPPFLAGS := -fexceptions -fPIC -std=c++14

LOCAL_LDFLAGS += -Wl,-s

# Export only luaopen_plugin_webm; localize all other symbols (incl. the static
# libc++ and libvpx/libwebm/opus) so they can't be interposed by the engine.
LOCAL_LDFLAGS += -Wl,--version-script=$(LOCAL_PATH)/export.map

# 16 KB page alignment only applies to the 64-bit ABIs (Android 15+); the flag
# is meaningless for the 32-bit ABIs (armeabi-v7a, x86).
ifneq (,$(filter $(TARGET_ARCH_ABI),arm64-v8a x86_64))
    LOCAL_LDFLAGS += -Wl,-z,max-page-size=16384
    LOCAL_LDFLAGS += -Wl,-z,common-page-size=16384
endif

LOCAL_SHARED_LIBRARIES := liblua libcorona libopenal

LOCAL_STATIC_LIBRARIES := libvpx libwebm libopus

ifeq ($(TARGET_ARCH), arm)
    LOCAL_CFLAGS+= -D_ARM_ASSEM_
endif

ifeq ($(TARGET_ARCH), x86)
    LOCAL_DISABLE_FATAL_LINKER_WARNINGS := true
endif

LOCAL_ARM_MODE := arm
include $(BUILD_SHARED_LIBRARY)

