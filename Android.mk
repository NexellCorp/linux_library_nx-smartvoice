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
include $(CLEAR_VARS)

LOCAL_MODULE := test-nx-voice

LOCAL_SRC_FILES := nx-smartvoice.cpp

LOCAL_C_INCLUDES += \
	frameworks/native/include		\
	system/core/include			\
	external/tinyalsa/include		\
	$(LOCAL_PATH)/../../library/libagcpdm	\
	$(LOCAL_PATH)/../../library/libresample \
	$(LOCAL_PATH)/../../library/libpowervoice

LOCAL_SHARED_LIBRARIES := \
	libcutils \
	libutils \
	libpowervoice

LOCAL_LDFLAGS += \
	-L$(LOCAL_PATH)/../../library/libagcpdm	\
	-lagcpdm \

LOCAL_SHARED_LIBRARIES += \
	libtinyalsa \
	libpowervoice

LOCAL_STATIC_LIBRARIES += \
	libresample

include $(BUILD_EXECUTABLE)
