PROFILE ?= buildroot-check
ELF2_SDK_HOME ?= $(HOME)/.local/opt/elf2-sdk

ifeq ($(PROFILE),buildroot-check)
PROFILE_STRICT := 0
ELF2_SDK_ROOT ?= $(ELF2_SDK_HOME)/aarch64-buildroot-linux-gnu_sdk-buildroot
TARGET_TRIPLE ?= aarch64-buildroot-linux-gnu
TOOLCHAIN_BIN ?= $(ELF2_SDK_ROOT)/bin
CROSS_COMPILE ?= $(TOOLCHAIN_BIN)/$(TARGET_TRIPLE)-
SYSROOT ?= $(ELF2_SDK_ROOT)/$(TARGET_TRIPLE)/sysroot
else ifeq ($(PROFILE),jammy-deploy)
PROFILE_STRICT := 1
TOOLCHAIN_ROOT ?= $(ELF2_SDK_HOME)/toolchains/gcc-arm-10.3-aarch64
TOOLCHAIN_BIN ?= $(TOOLCHAIN_ROOT)/bin
CROSS_COMPILE ?= $(TOOLCHAIN_BIN)/aarch64-none-linux-gnu-
SYSROOT ?= $(ELF2_SDK_HOME)/profiles/jammy-deploy/sysroot
else
$(error unknown ELF2 profile: $(PROFILE))
endif
