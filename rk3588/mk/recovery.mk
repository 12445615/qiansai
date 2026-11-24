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

.PHONY: recovery-print-config recovery-test recovery-compile-fail-test recovery-sanitize-test recovery-cross-check recovery-task4-boundary recovery-task5-boundary

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
	$(MAKE) recovery-task4-boundary
	$(MAKE) recovery-task5-boundary
	HOST_CXX="$(HOST_CXX)" CROSS_COMPILE="$(CROSS_COMPILE)" \
		RECOVERY_CROSS_CXX="$(CXX)" RECOVERY_SYSROOT="$(SYSROOT)" \
		$(ROOT_DIR)/tests/test_recovery_digest_probe.sh

recovery-task4-boundary:
	@if grep -En '#include[[:space:]]*<(sys/wait\.h|poll\.h)>|\b(waitid|poll)[[:space:]]*\(' \
		$(RECOVERY_INCLUDE_DIR)/recovery/runtime.hpp $(RECOVERY_SOURCE_DIR)/runtime.cc; then \
		echo "FAIL: Task 5 observation primitive leaked into Task 4 runtime" >&2; exit 1; \
	else \
		echo "PASS: Task 4 runtime contains no waitid/poll/<sys/wait.h>"; \
	fi
	@if sed -n '/struct RecoveryInstanceRuntime/,/^};/p' $(RECOVERY_INCLUDE_DIR)/recovery/runtime.hpp \
		| grep -En 'fault_batch_open|fault_batch_cutoff|fault_batch_digest'; then \
		echo "FAIL: duplicate fault-batch close state remains in RecoveryInstanceRuntime" >&2; exit 1; \
	else \
		echo "PASS: RecoveryInstanceRuntime has only the collecting fault_batch"; \
	fi
	@if sed -n '/struct RecoveryInstanceRuntime/,/^};/p' $(RECOVERY_INCLUDE_DIR)/recovery/runtime.hpp \
		| grep -En 'acked_owners|owner_ack_canonical_digest'; then \
		echo "FAIL: owner-ACK digest or acked set duplicated in RecoveryInstanceRuntime (R168)" >&2; exit 1; \
	else \
		echo "PASS: RecoveryInstanceRuntime keeps owner ACKs only as required_owners + accepted_owner_acks"; \
	fi
	@if grep -REn 'FaultReportToken|ReporterId|last_report_seq|fault_content_digest' \
		$(RECOVERY_INCLUDE_DIR)/recovery $(RECOVERY_SOURCE_DIR); then \
		echo "FAIL: deleted replay/dedup kernel state remains in recovery sources" >&2; exit 1; \
	else \
		echo "PASS: kernel recovery sources contain no replay-token state"; \
	fi

recovery-task5-boundary:
	@if grep -REn --exclude=proof.cc '#include[[:space:]]*<(sys/wait\.h|poll\.h)>|\b(waitid|poll)[[:space:]]*\(' \
		$(RECOVERY_SOURCE_DIR); then \
		echo "FAIL: process-exit observation primitive leaked outside proof.cc" >&2; exit 1; \
	else \
		echo "PASS: waitid/poll are isolated to proof.cc"; \
	fi
	@if ! grep -En '#include[[:space:]]*<(sys/wait\.h|poll\.h)>|\b(waitid|poll)[[:space:]]*\(' \
		$(RECOVERY_SOURCE_DIR)/proof.cc; then \
		echo "FAIL: proof.cc does not own the process-exit observation primitive" >&2; exit 1; \
	else \
		echo "PASS: proof.cc owns the process-exit observation primitive"; \
	fi

$(RECOVERY_TEST_BINARY): $(RECOVERY_TEST_OBJECTS) $(RECOVERY_PRODUCTION_OBJECTS)
	$(HOST_CXX) $(RECOVERY_CXXFLAGS) -o $@ $^ $(RECOVERY_LDLIBS)

$(RECOVERY_BUILD_DIR)/%.o: $(RECOVERY_TEST_DIR)/%.cc $(RECOVERY_TEST_DIR)/test_support.hpp
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(RECOVERY_CXXFLAGS) $(RECOVERY_TEST_CPPFLAGS) -I$(RECOVERY_TEST_DIR) -I$(RECOVERY_INCLUDE_DIR) -c $< -o $@

$(RECOVERY_BUILD_DIR)/src/%.o: $(RECOVERY_SOURCE_DIR)/%.cc
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(RECOVERY_CXXFLAGS) $(RECOVERY_TEST_CPPFLAGS) -I$(RECOVERY_INCLUDE_DIR) -c $< -o $@

-include $(RECOVERY_TEST_DEPFILES) $(RECOVERY_PRODUCTION_DEPFILES)
