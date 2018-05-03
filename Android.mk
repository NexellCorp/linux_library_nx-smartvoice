# Copyright (C) 2008 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
LOCAL_PATH := $(call my-dir)

ANDROID_VERSION_STR := $(subst ., ,$(PLATFORM_VERSION))
ANDROID_VERSION_MAJOR := $(firstword $(ANDROID_VERSION_STR))

# libnxvoice
include $(CLEAR_VARS)

ifeq "7" "$(ANDROID_VERSION_MAJOR)"
LOCAL_CFLAGS += -DNOUGAT=1
endif

# if you want to save raw pcm data, uncomment below cflags
# and make folder /data/tmp/
# you can check /data/tmp/ref.raw , pdm.raw, pdm2.raw
#LOCAL_CFLAGS += -DFILE_DUMP=1

LOCAL_MODULE :=libnxvoice
LOCAL_LDLIBS := -llog
LOCAL_SRC_FILES := \
	buffermanager.cpp \
	nx-smartvoice.cpp
LOCAL_C_INCLUDES += \
	external/tinyalsa/include \
	system/core/include \
	$(LOCAL_PATH)/../../library/libagcpdm \
	$(LOCAL_PATH)/../../library/libresample \
	$(LOCAL_PATH)/../../library/include
LOCAL_SHARED_LIBRARIES += \
	libtinyalsa \
	libcutils \
	libutils
LOCAL_STATIC_LIBRARIES += \
	libagcpdm \
	libresample

ifneq ($(filter pvo,$(SVOICE_ECNR_VENDOR)),)
LOCAL_SRC_FILES += \
	pvo.c
LOCAL_C_INCLUDES += \
	$(LOCAL_PATH)/../../library/libpowervoice
LOCAL_SHARED_LIBRARIES += \
	libpvo \
	libpovosource
endif

ifneq ($(filter mwsr,$(SVOICE_ECNR_VENDOR)),)
LOCAL_SRC_FILES += \
	mwsr.c
LOCAL_C_INCLUDES += \
	$(LOCAL_PATH)/../../library/libmwsr
LOCAL_SHARED_LIBRARIES += \
	libmwsr
endif

ifneq ($(filter bypass,$(SVOICE_ECNR_VENDOR)),)
LOCAL_SRC_FILES += \
	bypass.c
endif

include $(BUILD_SHARED_LIBRARY)

# test-svoice
include $(CLEAR_VARS)

ifeq "7" "$(ANDROID_VERSION_MAJOR)"
LOCAL_CFLAGS += -DNOUGAT=1
endif

LOCAL_MODULE := test-svoice
LOCAL_SRC_FILES := \
	buffermanager.cpp \
	test-svoice.cpp
LOCAL_C_INCLUDES += \
	system/core/include \
	$(LOCAL_PATH)/../../library/include
LOCAL_SHARED_LIBRARIES += \
	libcutils \
	libutils \
	libnxvoice

ifneq ($(filter pvo,$(SVOICE_ECNR_VENDOR)),)
LOCAL_C_INCLUDES += \
	$(LOCAL_PATH)/../../library/libpowervoice
LOCAL_SHARED_LIBRARIES += \
	libpvo \
	libpovosource
endif

ifneq ($(filter mwsr,$(SVOICE_ECNR_VENDOR)),)
LOCAL_C_INCLUDES += \
	$(LOCAL_PATH)/../../library/libmwsr
LOCAL_SHARED_LIBRARIES += \
	libmwsr
endif

LOCAL_LDLIBS := -llog
include $(BUILD_EXECUTABLE)
