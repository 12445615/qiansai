ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

include $(ROOT_DIR)/mk/elf2_profiles.mk
CC = $(CROSS_COMPILE)gcc
CXX = $(CROSS_COMPILE)g++

TARGET := main

C_SRCS := main.c camera.c ipc.c encoder.c aliyun_mqtt.c zone_detector.c \
          sensor_modbus.c local_store.c video_store.c video_uploader.c \
          osd_cache.c audio_alert.c safety_interlock_client.c
CXX_SRCS := postprocess.cc rknn_worker.cc
OBJS := $(C_SRCS:.c=.o) $(CXX_SRCS:.cc=.o)
DEPS := $(OBJS:.o=.d)

SYSROOT_INC := $(SYSROOT)/usr/include
USR_LOCAL_LIB_DIR := $(SYSROOT)/usr/local/lib
MULTIARCH_LIB_DIR := $(SYSROOT)/usr/lib/aarch64-linux-gnu
ROOT_MULTIARCH_LIB_DIR := $(SYSROOT)/lib/aarch64-linux-gnu
USR_LIB_DIR := $(SYSROOT)/usr/lib
LIB_SEARCH_DIRS := $(strip $(if $(wildcard $(USR_LOCAL_LIB_DIR)),$(USR_LOCAL_LIB_DIR)) $(if $(wildcard $(MULTIARCH_LIB_DIR)),$(MULTIARCH_LIB_DIR)) $(if $(wildcard $(ROOT_MULTIARCH_LIB_DIR)),$(ROOT_MULTIARCH_LIB_DIR)) $(if $(wildcard $(USR_LIB_DIR)),$(USR_LIB_DIR)))
SYSROOT_LIB ?= $(firstword $(LIB_SEARCH_DIRS))
SYSROOT_ARCH_INC ?= $(if $(wildcard $(SYSROOT_INC)/aarch64-linux-gnu),$(SYSROOT_INC)/aarch64-linux-gnu,$(SYSROOT_INC))

find_lib = $(firstword $(foreach libdir,$(LIB_SEARCH_DIRS),$(wildcard $(libdir)/$(1))))
AVCODEC_LIB := $(call find_lib,libavcodec.so)
RKNN_LIB := $(call find_lib,librknnrt.so)
RGA_LIB := $(call find_lib,librga.so)
PAHO_LIB := $(call find_lib,libpaho-mqtt3c.so)
MODBUS_LIB := $(call find_lib,libmodbus.so)
MPP_LIB := $(call find_lib,librockchip_mpp.so)
FFMPEG_ABI := $(shell if test -r "$(SYSROOT_INC)/libavcodec/version_major.h"; then sed -n 's/^#define LIBAVCODEC_VERSION_MAJOR[[:space:]]*//p' "$(SYSROOT_INC)/libavcodec/version_major.h"; fi)

PAHO_MQTT := $(if $(and $(wildcard $(SYSROOT_INC)/MQTTClient.h),$(PAHO_LIB)),enabled,disabled)
LIBMODBUS := $(if $(and $(wildcard $(SYSROOT_INC)/modbus/modbus.h),$(MODBUS_LIB)),enabled,disabled)
RKMPP_ENCODER := $(shell if test -n "$(AVCODEC_LIB)" -a -r "$(AVCODEC_LIB)"; then if strings "$(AVCODEC_LIB)" 2>/dev/null | grep -q h264_rkmpp; then echo available; else echo unavailable; fi; else echo unknown; fi)

CPPFLAGS := -I$(SYSROOT_INC) \
            -I$(SYSROOT_INC)/rknn \
            -I$(SYSROOT_INC)/rga \
            -I$(SYSROOT_INC)/freetype2 \
            -I$(SYSROOT_INC)/libdrm \
            -I$(SYSROOT_ARCH_INC) \
            -I.
CFLAGS := -Wall -Wextra -O2 -g -MMD -MP -DDISABLE_LIBJPEG
CXXFLAGS := $(CFLAGS) -std=c++11

LDFLAGS := --sysroot=$(SYSROOT) \
           $(foreach libdir,$(LIB_SEARCH_DIRS),-L$(libdir) -Wl,-rpath-link,$(libdir)) \
           -Wl,--allow-shlib-undefined
LDLIBS := -lavformat -lavcodec -lswresample -lavutil \
          -lcurl -lsqlite3 -ldrm -lrga -lrknnrt -lfreetype \
          -lm -lpthread -ldl

# The official ELF2 Buildroot SDK does not ship Paho MQTT C or libmodbus.
# The source already contains compile-time fallbacks when their headers are
# absent, so link them only when an augmented sysroot provides both libraries.
ifeq ($(PAHO_MQTT),enabled)
LDLIBS += -lpaho-mqtt3c
endif

ifeq ($(LIBMODBUS),enabled)
LDLIBS += -lmodbus
endif

.DEFAULT_GOAL := all

.PHONY: all clean check-env print-config test

all: check-env $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

%.o: %.cc
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

check-env:
	@test -x "$(CC)" || { echo "error: ELF2 cross compiler not found: $(CC)" >&2; echo "hint: run scripts/setup_elf2_sdk.sh or override ELF2_SDK_ROOT" >&2; exit 1; }
	@test -d "$(SYSROOT)" || { echo "error: ELF2 sysroot not found: $(SYSROOT)" >&2; exit 1; }
	@test -f "$(SYSROOT_INC)/rknn/rknn_api.h" || { echo "error: RKNN header missing from ELF2 sysroot" >&2; exit 1; }
	@test -n "$(RKNN_LIB)" -a -f "$(RKNN_LIB)" || { echo "error: RKNN runtime library missing from ELF2 sysroot" >&2; exit 1; }
	@test -f "$(SYSROOT_INC)/rga/im2d.h" || { echo "error: RGA header missing from ELF2 sysroot" >&2; exit 1; }
	@test -n "$(RGA_LIB)" -a -f "$(RGA_LIB)" || { echo "error: RGA runtime library missing from ELF2 sysroot" >&2; exit 1; }
	@echo "ELF2 SDK environment OK"
	@if test "$(PAHO_MQTT)" != enabled; then echo "warning: Paho MQTT C is unavailable; MQTT code will use its ENOSYS fallback" >&2; fi
	@if test "$(LIBMODBUS)" != enabled; then echo "warning: libmodbus is unavailable; Modbus code will use its ENOSYS fallback" >&2; fi
	@if test "$(RKMPP_ENCODER)" != available; then echo "warning: this sysroot does not expose the h264_rkmpp FFmpeg encoder; use the board/Jammy sysroot for deployment" >&2; fi

print-config:
	@echo "PROFILE=$(PROFILE)"
	@echo "PROFILE_STRICT=$(PROFILE_STRICT)"
	@echo "ELF2_SDK_ROOT=$(ELF2_SDK_ROOT)"
	@echo "SYSROOT=$(SYSROOT)"
	@echo "SYSROOT_LIB=$(SYSROOT_LIB)"
	@echo "LIB_SEARCH_DIRS=$(LIB_SEARCH_DIRS)"
	@echo "RKNN_LIB=$(RKNN_LIB)"
	@echo "RGA_LIB=$(RGA_LIB)"
	@echo "MPP_LIB=$(MPP_LIB)"
	@echo "FFMPEG_ABI=$(FFMPEG_ABI)"
	@echo "CC=$(CC)"
	@echo "CXX=$(CXX)"
	@echo "PAHO_MQTT=$(PAHO_MQTT)"
	@echo "LIBMODBUS=$(LIBMODBUS)"
	@echo "RKMPP_ENCODER=$(RKMPP_ENCODER)"
	@echo "CPPFLAGS=$(CPPFLAGS)"
	@echo "LDLIBS=$(LDLIBS)"

test:
	bash tests/test_profile_config.sh
	bash tests/test_build_wrapper.sh
	bash tests/test_capture_rockchip_tools.sh
	bash tests/test_jammy_setup.sh
	bash tests/test_jammy_deps.sh
	bash tests/test_ffmpeg6_rkmpp_config.sh
	bash tests/test_build_config.sh
	bash tests/test_sdk_setup.sh
	bash tests/test_optional_deps_build.sh

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

-include $(DEPS)
