HOST_CXX ?= g++

RECOVERY_CXX_STANDARD := c++20
RECOVERY_CXXFLAGS := -std=$(RECOVERY_CXX_STANDARD) -Wall -Wextra -Wpedantic -Werror -MMD -MP
RECOVERY_TEST_CPPFLAGS := -DRECOVERY_TESTING
RECOVERY_BUILD_DIR := $(ROOT_DIR)/build/host/recovery
RECOVERY_TEST_DIR := $(ROOT_DIR)/tests/recovery
RECOVERY_INCLUDE_DIR := $(ROOT_DIR)/recovery/include
RECOVERY_SOURCE_DIR := $(ROOT_DIR)/recovery/src
RECOVERY_TEST_SOURCES := $(wildcard $(RECOVERY_TEST_DIR)/test_*.cc)
RECOVERY_PRODUCTION_SOURCES := $(wildcard $(RECOVERY_SOURCE_DIR)/*.cc)
RECOVERY_TEST_OBJECTS := $(patsubst $(RECOVERY_TEST_DIR)/%.cc,$(RECOVERY_BUILD_DIR)/%.o,$(RECOVERY_TEST_SOURCES))
RECOVERY_PRODUCTION_OBJECTS := $(patsubst $(RECOVERY_SOURCE_DIR)/%.cc,$(RECOVERY_BUILD_DIR)/src/%.o,$(RECOVERY_PRODUCTION_SOURCES))
RECOVERY_TEST_DEPFILES := $(RECOVERY_TEST_OBJECTS:.o=.d)
RECOVERY_PRODUCTION_DEPFILES := $(RECOVERY_PRODUCTION_OBJECTS:.o=.d)
RECOVERY_TEST_BINARY := $(RECOVERY_BUILD_DIR)/recovery_tests
RECOVERY_LDLIBS := -lcrypto

.PHONY: recovery-print-config recovery-test recovery-compile-fail-test recovery-sanitize-test recovery-cross-check

recovery-print-config:
	@echo "RECOVERY_CXX_STANDARD=$(RECOVERY_CXX_STANDARD)"
	@echo "HOST_CXX=$(HOST_CXX)"
	@echo "RECOVERY_BUILD_DIR=$(RECOVERY_BUILD_DIR)"

recovery-test: $(RECOVERY_TEST_BINARY)
	$(RECOVERY_TEST_BINARY)

recovery-compile-fail-test:
	HOST_CXX="$(HOST_CXX)" $(ROOT_DIR)/tests/test_recovery_compile_fail.sh

recovery-sanitize-test:
	ASAN_OPTIONS=detect_leaks=0 $(MAKE) -B RECOVERY_BUILD_DIR=$(RECOVERY_BUILD_DIR)/sanitize \
		RECOVERY_CXXFLAGS="$(RECOVERY_CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer" recovery-test

recovery-cross-check:
	$(ROOT_DIR)/tests/test_recovery_task3_boundary.sh
	HOST_CXX="$(HOST_CXX)" CROSS_COMPILE="$(CROSS_COMPILE)" \
		RECOVERY_CROSS_CXX="$(CXX)" RECOVERY_SYSROOT="$(SYSROOT)" \
		$(ROOT_DIR)/tests/test_recovery_digest_probe.sh

$(RECOVERY_TEST_BINARY): $(RECOVERY_TEST_OBJECTS) $(RECOVERY_PRODUCTION_OBJECTS)
	$(HOST_CXX) $(RECOVERY_CXXFLAGS) -o $@ $^ $(RECOVERY_LDLIBS)

$(RECOVERY_BUILD_DIR)/%.o: $(RECOVERY_TEST_DIR)/%.cc $(RECOVERY_TEST_DIR)/test_support.hpp
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(RECOVERY_CXXFLAGS) $(RECOVERY_TEST_CPPFLAGS) -I$(RECOVERY_TEST_DIR) -I$(RECOVERY_INCLUDE_DIR) -c $< -o $@

$(RECOVERY_BUILD_DIR)/src/%.o: $(RECOVERY_SOURCE_DIR)/%.cc
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(RECOVERY_CXXFLAGS) $(RECOVERY_TEST_CPPFLAGS) -I$(RECOVERY_INCLUDE_DIR) -c $< -o $@

-include $(RECOVERY_TEST_DEPFILES) $(RECOVERY_PRODUCTION_DEPFILES)
