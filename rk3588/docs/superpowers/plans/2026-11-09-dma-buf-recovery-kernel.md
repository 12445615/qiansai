# DMA-BUF Recovery and Proof Kernel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the frozen DMA-BUF recovery/proof kernel as an isolated C++20 module, prove its host-side state-machine contracts with R1–R161 traceable tests, then verify the Linux/RK3588 capabilities in §19 before connecting it to the existing media pipeline.

**Architecture:** Keep the current C/C++11 application build unchanged. Add a separate C++20 recovery module with five dependency layers: core/canonical encoding, immutable compiler, recovery runtime and proof barrier, authorization/grant/authentication, then Linux IPC and hardware adapters. Host tests run without RK3588 hardware; target probes keep every capability `UNVERIFIED` until evidence is captured. No new architecture layer and no edits to the frozen design are allowed.

**Tech Stack:** GNU Make, C++20, GCC 11.4 host compiler, GCC 12.3 AArch64 cross compiler, POSIX/Linux APIs, OpenSSL candidate for SHA-256/HMAC after capability verification, Bash capability probes, ASan/UBSan/TSan.

---

## Authoritative input and non-negotiable constraints

- Frozen design: `/mnt/e/Obsidian_warehouse/嵌入式/1-Project/RK3588工业安全监测/2026-11-02-DMA-BUF恢复与证明内核设计.md`
- Frozen SHA-256: `8B7C5DFD8AE8753CA05A06D130EC995735AB2B89B068861745B14BF5A265D9A8`
- Implementation repository: `/home/chx/Linux/qiansai/rk3588`
- Existing branch at planning time: `codex/elf2-build-env`
- Existing modified/untracked files belong to the user. Never reset, delete, overwrite, or include them through `git add -A`.
- The design document is read-only. Implementation conflicts are reported as blockers; they are not silently resolved by changing the design.
- Each behavior uses strict RED → verify expected failure → minimal GREEN → full regression → refactor.
- For one design contract, three failed correction rounds are the limit. After the third failure, stop and report `DESIGN_BLOCKER` with the design clause, failing test, observed behavior, attempted fixes, and required design decision. Do not perform a fourth rebuild.
- R1–R161 are evidence identifiers. Preserve the R number in test names and the traceability matrix even when implementation order differs.
- §19 items remain `UNVERIFIED` until a command, target/kernel version, raw output, and conclusion are recorded. Unsupported capability means the dependent DeviceTarget path returns `BOARD_RESTART_REQUIRED`; never fabricate a success proof.

## Scope and file map

Create the recovery module without moving existing application files:

- `recovery/include/recovery/types.hpp`
- `recovery/include/recovery/canonical.hpp`
- `recovery/include/recovery/crypto.hpp`
- `recovery/include/recovery/program.hpp`
- `recovery/include/recovery/compiler.hpp`
- `recovery/include/recovery/runtime.hpp`
- `recovery/include/recovery/proof.hpp`
- `recovery/include/recovery/authorization.hpp`
- `recovery/include/recovery/grant_ledger.hpp`
- `recovery/include/recovery/auth.hpp`
- `recovery/include/recovery/ipc.hpp`
- `recovery/src/canonical.cc`
- `recovery/src/crypto_openssl.cc`
- `recovery/src/compiler.cc`
- `recovery/src/runtime.cc`
- `recovery/src/proof.cc`
- `recovery/src/authorization.cc`
- `recovery/src/grant_ledger.cc`
- `recovery/src/auth.cc`
- `recovery/src/ipc_linux.cc`
- `mk/recovery.mk`
- `tests/recovery/test_support.hpp`
- `tests/recovery/test_main.cc`
- `tests/recovery/test_*.cc`
- `tests/recovery/compile_fail/*.cc`
- `tests/probes/recovery_*.cc`
- `tests/test_recovery_*.sh`
- `docs/recovery/recovery-design-baseline.md`
- `docs/recovery/recovery-traceability.md`
- `docs/recovery/hardware-capability-evidence.md`

Do not add recovery sources to the existing `main` target until Tasks 1–10 are green.

## Required Make targets

The completed plan exposes these independent commands:

```bash
make recovery-test                 # host behavior suite
make recovery-compile-fail-test    # private-constructor/mutator negative compilation
make recovery-sanitize-test        # ASan + UBSan
make recovery-tsan-test            # concurrency/lock tests
make recovery-cross-check          # AArch64 compile/link probe; no board execution
make recovery-capability-test      # §19 probes; may report UNVERIFIED/SKIP
make recovery-test-all             # all host gates, excluding unavailable hardware
```

`make test` may call `recovery-test-all` only after the new suite is stable; do not break the current shell tests during bootstrap.

### Task 1: Freeze the input and bootstrap an isolated C++20 test target

**Files:**
- Create: `docs/recovery/recovery-design-baseline.md`
- Create: `docs/recovery/recovery-traceability.md`
- Create: `tests/test_recovery_design_baseline.sh`
- Create: `tests/test_recovery_build.sh`
- Create: `tests/recovery/test_support.hpp`
- Create: `tests/recovery/test_main.cc`
- Create: `tests/recovery/test_smoke.cc`
- Create: `mk/recovery.mk`
- Modify: `Makefile`

- [ ] **Step 1: Record the dirty-tree boundary**

Run and save the output in the WorkBuddy execution report, not in a generated file:

```bash
cd /home/chx/Linux/qiansai/rk3588
git status --short --branch
```

Expected: the existing `.gitignore`, `aliyun_mqtt.c`, `encoder.c/.h`, interview/technical documents, SDK scripts, and tests remain untouched.

- [ ] **Step 2: Write the failing baseline and build-contract tests**

`tests/test_recovery_design_baseline.sh` must:

1. accept `RECOVERY_DESIGN_DOC`, defaulting to the authoritative path above;
2. verify the exact SHA-256;
3. verify headings `## 18. 反向测试矩阵`, `## 19. 真机 capability 待验证清单`, and `## 20. 编译闭合检查清单`;
4. fail if any input is missing or changed.

`tests/test_recovery_build.sh` must require `recovery-print-config` and `recovery-test`, assert `RECOVERY_CXX_STANDARD=c++20`, and reject accidental use of `$(CXXFLAGS)` with `-std=c++11`.

Run:

```bash
bash tests/test_recovery_design_baseline.sh
bash tests/test_recovery_build.sh
```

Expected RED: baseline manifest and recovery Make targets do not exist.

- [ ] **Step 3: Add a zero-dependency test runner**

Implement a small standard-library runner in `test_support.hpp`/`test_main.cc` with:

- `RECOVERY_TEST(name)` registration;
- `RECOVERY_CHECK(expr)` and equality checking with file/line output;
- `--list` and `--filter <substring>`;
- one `[PASS]`/`[FAIL]` line per test and non-zero exit if any test fails.

No production header may depend on this test support.

- [ ] **Step 4: Add the isolated Make fragment**

`mk/recovery.mk` must use `HOST_CXX ?= g++`, its own `-std=c++20 -Wall -Wextra -Wpedantic -Werror`, and output only below `build/host/recovery/`. Include it from the root `Makefile` without changing the existing application's C++11 flags.

Run:

```bash
bash tests/test_recovery_design_baseline.sh
bash tests/test_recovery_build.sh
make recovery-test
make test
```

Expected GREEN: recovery smoke test passes; every pre-existing shell test still passes.

- [ ] **Step 5: Create the initial traceability table**

`docs/recovery/recovery-traceability.md` must contain columns `ID | design clause | automated test | environment | status | evidence`. Initialize R1–R161 and §19.1–§19.21 as `NOT_RUN`/`UNVERIFIED`; do not mark anything PASS merely because a test file exists.

- [ ] **Step 6: Commit only owned files**

```bash
git -C /home/chx/Linux/qiansai add -- \
  rk3588/Makefile rk3588/mk/recovery.mk \
  rk3588/tests/test_recovery_design_baseline.sh \
  rk3588/tests/test_recovery_build.sh rk3588/tests/recovery \
  rk3588/docs/recovery \
  rk3588/docs/superpowers/plans/2026-11-09-dma-buf-recovery-kernel.md
git -C /home/chx/Linux/qiansai diff --cached --check
git -C /home/chx/Linux/qiansai commit -m "test: bootstrap recovery kernel verification"
```

Confirm the root first with `git rev-parse --show-toplevel`; never compensate for a path error with `git add -A`.

### Task 2: Implement strong types, move-only handles, and canonical bytes

**Files:**
- Create: `recovery/include/recovery/types.hpp`
- Create: `recovery/include/recovery/canonical.hpp`
- Create: `recovery/src/canonical.cc`
- Create: `tests/recovery/test_types.cc`
- Create: `tests/recovery/test_canonical.cc`
- Create: `tests/recovery/compile_fail/permit_move_assign.cc`
- Create: `tests/test_recovery_compile_fail.sh`
- Modify: `mk/recovery.mk`
- Modify: `docs/recovery/recovery-traceability.md`

- [ ] Write RED tests for typed ID non-interchangeability, `ProcessPrincipal`/`CoordinatorPrincipal`, move-invalidating `PermitHandle`, length-delimited canonical encoding, deterministic ordering, and `[ab,c] != [a,bc]` (R42, R52, R59, R75, R76, R126).
- [ ] Run `make recovery-test` and confirm failures are missing types/behavior, not test syntax errors.
- [ ] Implement only the core POD/variant types and canonical byte writer needed by those tests. No compiler/runtime state yet.
- [ ] Add negative compilation for deleted move assignment and forbidden cross-type construction; run `make recovery-compile-fail-test`.
- [ ] Run `make recovery-test recovery-compile-fail-test recovery-sanitize-test`.
- [ ] Update traceability only for tests actually executed, then commit owned files.

### Task 3: Pin the digest implementation and immutable compiler output

**Files:**
- Create: `recovery/include/recovery/crypto.hpp`
- Create: `recovery/src/crypto_openssl.cc`
- Create: `recovery/include/recovery/program.hpp`
- Create: `recovery/include/recovery/compiler.hpp`
- Create: `recovery/src/compiler.cc`
- Create: `tests/recovery/test_crypto_vectors.cc`
- Create: `tests/recovery/test_compiler.cc`
- Create: `tests/recovery/compile_fail/program_construct.cc`
- Create: `tests/recovery/compile_fail/program_mutate.cc`
- Modify: `mk/recovery.mk`
- Modify: `docs/recovery/recovery-traceability.md`

- [ ] Before implementation, add a probe proving host and Buildroot sysroot can compile/link the selected 32-byte digest library. If target linking fails, stop this task as an environment blocker; do not write a second hash algorithm.
- [ ] Write RED golden-vector tests for `canonical_digest` and deterministic compilation, plus negative compilation for constructing/mutating `ValidatedRecoveryProgram` (R1–R9, R43, R44, R56).
- [ ] Implement SHA-256 behind `canonical_digest` with no algorithm selector in public APIs. Use the audited library, not a handwritten hash.
- [ ] Implement `RecoveryCompiler::compile`, private program construction, action closure failures, predicate/target exact matching, and transaction sealing exactly as §§7–9 and §15 require.
- [ ] Add allocation-failure injection through an explicit allocator/failure-provider boundary, never through test-only public mutators; cover R108, R109, R110, R127, R138.
- [ ] Run:

```bash
make recovery-test
make recovery-compile-fail-test
make recovery-sanitize-test
make recovery-cross-check
```

Expected: deterministic hashes and programs; all forbidden construction tests fail compilation for the intended access/type reason.

### Task 4: Implement fault batching, fallback, and per-recovery ownership

**Files:**
- Create: `recovery/include/recovery/runtime.hpp`
- Create: `recovery/src/runtime.cc`
- Create: `tests/recovery/test_fault_batch.cc`
- Create: `tests/recovery/test_runtime_lifecycle.cc`
- Create: `tests/recovery/test_fallback.cc`
- Modify: `docs/recovery/recovery-traceability.md`

- [ ] Write RED tests for the 12 transitions in §§4–13 beginning with `report_fault`, `begin_freeze`, `commit_freeze`, compile/fallback/seal, quiesce, commit, and reopen.
- [ ] First cover fault ordering/deduplication and closed-batch behavior (R10–R14), then fallback severity (R15–R19, R39–R41).
- [ ] Implement `RecoverySlot` boot-global state and the single optional `RecoveryInstanceRuntime`; publish a fully prepared runtime atomically and destroy it only with `active.reset()` (R64, R70, R78, R88–R92, R102, R103).
- [ ] Add OOM/failure-provider cases before each state mutation and assert byte-for-byte unchanged committed state on failure.
- [ ] Run filtered tests after every state transition, then the full host/sanitizer suite.

### Task 5: Implement proof matching and the exact proof barrier

**Files:**
- Create: `recovery/include/recovery/proof.hpp`
- Create: `recovery/src/proof.cc`
- Create: `tests/recovery/test_proof_matching.cc`
- Create: `tests/recovery/test_proof_barrier.cc`
- Modify: `docs/recovery/recovery-traceability.md`

- [ ] Write RED cases R20–R25, R45–R50, R57–R59, and R63, including cross-recovery replay, duplicate predicate key, wrong variant alternative, stale/revoked evidence, mismatched pidfd generation/fence context/reset generation, and unverified device proof.
- [ ] Implement `VerificationContext` construction only from a frozen registry snapshot; implement `ProofIndex` keyed by `PredicateKey` and exact-set barrier comparison.
- [ ] Keep DeviceTarget proofs that depend on §19 capabilities rejected or `BOARD_RESTART_REQUIRED` until target evidence exists.
- [ ] Run `make recovery-test recovery-sanitize-test`; record only observed PASS results.

### Task 6: Implement authorization journal, idempotent claim/poll, and receipt commit

**Files:**
- Create: `recovery/include/recovery/authorization.hpp`
- Create: `recovery/src/authorization.cc`
- Create: `tests/recovery/test_authorization_advance.cc`
- Create: `tests/recovery/test_claim_poll.cc`
- Create: `tests/recovery/test_receipt_commit.cc`
- Create: `tests/recovery/compile_fail/journal_mutator_access.cc`
- Modify: `docs/recovery/recovery-traceability.md`

- [ ] RED: preallocation, two-phase authorization advance, nonce exhaustion, exact eligible set, no duplicate entry, private journal mutators, and no direct COMMIT/REOPEN injection (R26–R31, R52–R54, R60, R65–R69, R78–R87, R91, R93–R111).
- [ ] Implement stage A as allocation/failure-capable with zero journal mutation; implement stage B using only statically proven nothrow moves/scalars.
- [ ] Implement permanent per-recovery `RequestIndex`, idempotent `(ProcessPrincipal, DeliveryRequestId)` behavior, `DeliveryId`, claim/poll separation, and consume-in-place handle invalidation.
- [ ] Test every allocation failure before success tests; no public API may leak `bad_alloc`.
- [ ] Run host, negative-compilation, ASan/UBSan tests; do not begin epoch commit until this suite is green.

### Task 7: Implement epoch commit, single reopen, and cross-recovery isolation

**Files:**
- Modify: `recovery/include/recovery/runtime.hpp`
- Modify: `recovery/src/runtime.cc`
- Create: `tests/recovery/test_epoch_commit.cc`
- Create: `tests/recovery/test_reopen.cc`
- Create: `tests/recovery/test_cross_recovery.cc`
- Modify: `docs/recovery/recovery-traceability.md`

- [ ] RED: old-epoch mismatch, shared domain agreement, untouched-domain immutability, overflow, atomic receipt+epoch commit, duplicate commit, unique COMMIT/REOPEN path, exact prerequisite receipts, and old lease/runtime rejection (R32–R38, R51, R55, R62, R69, R71–R77, R87–R92, R101, R102, R104).
- [ ] Implement `commit_epoch` and `commit_reopen` with the design's prepared-result/nothrow-commit split. There must be no failure return after committed state starts changing.
- [ ] Run two full recoveries in one test process and assert boot-global monotonic counters survive while all per-recovery objects are new.
- [ ] Run `make recovery-test recovery-sanitize-test` and update traceability.

### Task 8: Implement principals, GrantLedger, and authenticated contexts

**Files:**
- Create: `recovery/include/recovery/grant_ledger.hpp`
- Create: `recovery/src/grant_ledger.cc`
- Create: `recovery/include/recovery/auth.hpp`
- Create: `recovery/src/auth.cc`
- Create: `tests/recovery/test_grant_ledger.cc`
- Create: `tests/recovery/test_executor_binding.cc`
- Create: `tests/recovery/test_auth_context.cc`
- Create: `tests/probes/recovery_crypto_probe.cc`
- Create: `tests/recovery/compile_fail/auth_context_construct.cc`
- Create: `tests/recovery/compile_fail/raw_auth_mutator_access.cc`
- Create: `tests/recovery/compile_fail/ticket_forge.cc`
- Modify: `docs/recovery/recovery-traceability.md`

- [ ] RED: R112–R143, split into authentication boundary, source/destination binding, closure executor selection, connection table concurrency, grant prepare/commit, and ticket forgery groups.
- [ ] Before implementing `compute_ticket_mac`, make `recovery_crypto_probe.cc` run on the host and cross-compile/link against the target sysroot, proving HMAC-SHA256 and `CRYPTO_memcmp` symbols are available. This is only an implementation prerequisite; §19 remains `UNVERIFIED` until the same binary runs on the RK3588 rootfs.
- [ ] Use OpenSSL HMAC-SHA256 and `CRYPTO_memcmp`. Do not implement crypto or byte-comparison loops manually. If either host or target-sysroot probe fails, keep key arming/grant issuance on the explicit unavailable path and stop this task as a capability blocker.
- [ ] Run `make recovery-test recovery-compile-fail-test recovery-sanitize-test recovery-tsan-test recovery-cross-check`.

### Task 9: Implement owned Linux IPC and session lifecycle

**Files:**
- Create: `recovery/include/recovery/ipc.hpp`
- Create: `recovery/src/ipc_linux.cc`
- Create: `tests/recovery/test_owned_fd.cc`
- Create: `tests/recovery/test_ipc_acceptor.cc`
- Create: `tests/recovery/test_session_lifecycle.cc`
- Create: `tests/recovery/test_ipc_shutdown.cc`
- Create: `tests/recovery/compile_fail/acceptor_private_create.cc`
- Modify: `docs/recovery/recovery-traceability.md`

- [ ] RED: R144–R161, using real AF_UNIX/SOCK_SEQPACKET sockets where possible and syscall/failure-provider injection only at OS boundaries.
- [ ] Implement `OwnedFd`, `OwnedConnection`, `ChannelSession`, `AuthenticatedSession`, factory-only coordinator creation, four-state IPC lifecycle, explicit shutdown, and the single failure-cleanup entrance.
- [ ] Verify every accept failure returns fd count to baseline. Verify moved-from/double-close paths, all six handler exits, shutdown timeout/retry, and five terminal cleanup failures.
- [ ] Run R150's concurrency loop under TSan and lock-order checking. If sanitizer runtime cannot support 100,000 rounds, keep the required total across deterministic shards and record exact counts.
- [ ] R160/R161 stay `UNVERIFIED` unless the watchdog/reboot outcome is externally observable; a mocked boolean is not sufficient evidence for the production terminal path.

### Task 10: Build and execute the §19 capability probes

**Files:**
- Create: `tests/probes/recovery_pidfd_probe.cc`
- Create: `tests/probes/recovery_seqpacket_probe.cc`
- Modify: `tests/probes/recovery_crypto_probe.cc`
- Create: `tests/probes/recovery_fd_leak_probe.cc`
- Create: `tests/probes/recovery_watchdog_probe.cc`
- Create: `tests/probes/recovery_media_capability_probe.cc`
- Create: `tests/test_recovery_capabilities.sh`
- Modify: `mk/recovery.mk`
- Modify: `docs/recovery/hardware-capability-evidence.md`
- Modify: `docs/recovery/recovery-traceability.md`

- [ ] Cross-compile all probes first; compile success is not runtime verification.
- [ ] On the RK3588 target, capture kernel release, rootfs image/build identity, relevant library versions/hashes, command, exit status, and raw output for each §19.1–§19.21 item.
- [ ] Test `waitid(P_PIDFD)`, pid reuse/starttime, SO_PEERCRED/user namespace, CSPRNG startup behavior, SEQPACKET framing/MSG_TRUNC/buffer limits, poll/HUP/ERR shutdown latency, fd leak paths, OpenSSL HMAC/constant-time function, RGA/MPP/RKNN semantics, V4L2 orphaned buffers, reset evidence, and watchdog terminal observability.
- [ ] Mark only observed capabilities `VERIFIED`; use `UNSUPPORTED` with the designed fallback where appropriate. A skipped or unavailable probe remains `UNVERIFIED`.
- [ ] Re-run the dependent host tests with the capability manifest and ensure no unverified DeviceTarget emits success proof.

### Task 11: Close R1–R161 and prepare integration, without changing the architecture

**Files:**
- Modify: `docs/recovery/recovery-traceability.md`
- Create: `docs/recovery/recovery-verification-report.md`
- Create: `docs/recovery/media-integration-map.md`
- Modify only if all previous gates pass: `Makefile`

- [ ] Run:

```bash
make recovery-test-all
make recovery-cross-check
make recovery-capability-test
git diff --check
```

- [ ] Verify every R1–R161 row has a test name, environment, result, and evidence reference. `NOT_RUN`, `UNVERIFIED`, or `SKIP` is not PASS.
- [ ] Generate an integration map from existing `camera.c`, `encoder.c`, `rknn_worker.cc`, `ipc.c`, and resource ownership paths to the new recovery APIs. Do not edit those application files in this plan.
- [ ] Propose the smallest first vertical integration slice separately: one real fault → frozen snapshot → compiled program → one local action → receipt → proof barrier → commit → reopen.
- [ ] Request Codex review of the implementation evidence before any application integration. Integration begins only after the reference kernel and required target capabilities pass.

## Per-task WorkBuddy report contract

After every task, return exactly:

1. files changed;
2. RED command and the expected failure observed;
3. GREEN commands and full summary counts;
4. R/§19 rows advanced, with evidence paths;
5. remaining `UNVERIFIED` items;
6. deviations from the frozen design (normally `none`);
7. correction-attempt count for each unresolved behavior;
8. `git status --short` proving unrelated existing changes were preserved.

Do not start the next task until the current report has been reviewed.
