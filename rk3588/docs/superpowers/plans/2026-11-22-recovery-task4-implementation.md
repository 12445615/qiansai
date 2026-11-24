# Recovery Task 4 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the paused Task 4 validation implementation with the approved fault-batch single-source-of-truth model, private kernel-issued fault identities, and atomic freeze publication.

**Architecture:** `RecoveryInstanceRuntime::fault_batch` remains the only collecting batch. `prepare_fault_batch_close()` builds a local canonical close value, while `commit_freeze()` constructs all bindings and the snapshot in Phase A and publishes only `registry -> snapshot -> FROZEN` in Phase B. `RecoverySlotState` is the batch-open/closed fact; no replay or content deduplication exists in the kernel.

**Tech Stack:** C++20, g++, OpenSSL canonical digest, host unit tests, compile-fail probes, ASan/UBSan.

---

### Task 1: Establish the new public runtime contract

**Files:**
- Modify: `recovery/include/recovery/runtime.hpp`
- Test: `tests/recovery/test_fault_batch_freeze.cc`
- Test: `tests/recovery/compile_fail/program_construct.cc`
- Test: `tests/recovery/compile_fail/owned_process_exit_canonical.cc`

- [x] **Step 1: Add RED API/identity tests.** Add tests that call `report_fault(FaultReport)`, require two byte-identical reports to produce two facts with sequences 1 and 2, and verify the public header no longer exposes runtime duplicate close fields.
- [x] **Step 2: Run the focused test and compile-fail harness.** Run `make -B recovery-test` and `make recovery-compile-fail-test`; record the expected compile/API failures before changing production code.
- [x] **Step 3: Implement the contract.** Make `FaultIdentity` privately constructible, friend only the coordinator and testing seam under `RECOVERY_TESTING`; define `FaultReport` before `FaultIdentity` and `FaultFact`; change `report_fault` to accept `const FaultReport&`; remove `fault_batch_open`, `fault_batch_cutoff`, and `fault_batch_digest` from the runtime.
- [x] **Step 4: Re-run the focused tests.** Verify the new calls compile and old identity/token construction paths fail for the intended diagnostics.

### Task 2: Make fault reporting transactional and exception-safe

**Files:**
- Modify: `recovery/include/recovery/runtime.hpp`
- Modify: `recovery/src/runtime.cc`
- Modify: `tests/recovery/test_fault_batch_freeze.cc`
- Modify: `tests/recovery/test_fault_batch.cc`

- [x] **Step 1: Add RED tests for issuance and append failure.** Cover first issuance from counter zero, rejected-state no-consumption, failed deep copy/push, and `fault_seq_counter` plus `active.fault_batch` byte preservation.
- [x] **Step 2: Run the focused RED tests.** Run `make -B recovery-test`; confirm failures identify the old `FaultFact` API or old append semantics.
- [x] **Step 3: Implement `report_fault(const FaultReport&) noexcept`.** Under the coordinator lock, build a local fact with `{slot_.boot_id, slot_.fault_seq_counter + 1}`; publish the new runtime or append the fact first, then commit the counter; catch allocation failures as `FAIL_CLOSED`; preserve the counter and batch on every failed operation; do not deduplicate identical content.
- [x] **Step 4: Re-run the focused tests.** Verify successful identities are strictly increasing and all failure cases terminate inside the API without `std::terminate`.

### Task 3: Replace close mutation with pure batch preparation

**Files:**
- Modify: `recovery/include/recovery/runtime.hpp`
- Modify: `recovery/src/runtime.cc`
- Test: `tests/recovery/test_fault_batch_freeze.cc`

- [x] **Step 1: Add RED tests for batch preparation.** Cover empty batch rejection, boot mismatch, duplicate identity, non-increasing canonical order, sequence above cutoff, last sequence not equal to cutoff, and exact canonical digest/cutoff on success.
- [x] **Step 2: Run the RED tests.** Run the new filtered test through `build/host/recovery/recovery_tests` and confirm the helper is absent or the old fields make the assertions fail.
- [x] **Step 3: Implement `PreparedFaultBatchClose` and `prepare_fault_batch_close()`.** Capture the cutoff once from `slot_.fault_seq_counter`; copy and canonical-sort the active batch into locals; validate non-empty, boot, sequence bounds, uniqueness, strict order, and final sequence; calculate the local digest; never write `slot_.active`.
- [x] **Step 4: Re-run the helper tests.** Verify all malformed batches return `nullopt` and leave the collecting batch unchanged.

### Task 4: Rebuild freeze publication as two phases

**Files:**
- Modify: `recovery/src/runtime.cc`
- Modify: `tests/recovery/test_fault_batch_freeze.cc`
- Modify: `tests/recovery/test_process_exit_freeze.cc`
- Modify: `tests/recovery/test_process_exit_ownership.cc`
- Modify: `mk/recovery.mk`

- [x] **Step 1: Add RED publication tests.** Assert successful snapshots contain the prepared batch, cutoff, digest, descriptors, and registry; assert every Phase A failure leaves snapshot/registry/batch/counter unchanged and closes local descriptors.
- [x] **Step 2: Run the RED publication matrix.** Run `make -B recovery-test recovery-sanitize-test`; confirm failures expose old pre-publication writes or mismatched fields.
- [x] **Step 3: Implement local Phase A.** Build owner validation, prefix digest, prepared batch, target set, before/after generation reads, pidfd registry, snapshot, legality checks, descriptor/registry agreement, and snapshot digest entirely in locals. Use exactly two generation reads and no Task 5 observation primitive.
- [x] **Step 4: Implement nothrow Phase B.** Add static assertions and assign only `process_exit_binding_registry`, `current_snapshot`, then `slot_.state = FROZEN`; reject re-entry; let `OwnedFd` destructors close local fds on all failures.
- [x] **Step 5: Re-run host and sanitizer publication tests.** Verify atomic visibility and fd baselines.

### Task 5: Update compile-fail probes, build gates, and traceability

**Files:**
- Modify: `tests/test_recovery_compile_fail.sh`
- Modify: `tests/recovery/compile_fail/owned_process_exit_canonical.cc`
- Modify: `mk/recovery.mk`
- Modify: `docs/recovery/recovery-traceability.md`
- Modify: `docs/recovery/recovery-design-baseline.md`

- [x] **Step 1: Add exact diagnostic expectations.** Require each compile-fail source to declare and match its intended diagnostic; ensure the fd-carrying type fails specifically with `no matching function for call to 'canonical_write'`.
- [x] **Step 2: Run the compile-fail and boundary checks.** Run `make recovery-compile-fail-test recovery-task4-boundary`; fix only intended diagnostics and source-boundary violations.
- [ ] **Step 3: Re-freeze the baseline.** The external design remains `INVALIDATED / PENDING_REVIEW`; no frozen hash was invented.
- [x] **Step 4: Record evidence.** Update R10–R13, R137, R163, and R165 with fresh candidate evidence while keeping the unfrozen status explicit.

### Task 6: Full verification

**Files:**
- No additional source changes unless a verification failure identifies one of the above contracts.

- [x] Run `make -B recovery-test`.
- [x] Run `make recovery-compile-fail-test`.
- [x] Run `make recovery-sanitize-test`.
- [x] Run `make recovery-cross-check`.
- [x] Run `git diff --check` and inspect the final diff for unrelated changes.
