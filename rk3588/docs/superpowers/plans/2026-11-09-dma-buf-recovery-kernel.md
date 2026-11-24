# DMA-BUF Recovery and Proof Kernel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the DMA-BUF recovery/proof kernel as an isolated C++20 module, prove its host-side state-machine contracts with R1–R165 traceable tests, then verify the Linux/RK3588 capabilities in §19 before connecting it to the existing media pipeline.

**Architecture:** Keep the current C/C++11 application build unchanged. Add a separate C++20 recovery module with five dependency layers: core/canonical encoding, immutable compiler, recovery runtime and proof barrier, authorization/grant/authentication, then Linux IPC and hardware adapters. Host tests run without RK3588 hardware; target probes keep every capability `UNVERIFIED` until evidence is captured. No new architecture layer and no edits to the design document are allowed.

**Tech Stack:** GNU Make, C++20, GCC 11.4 host compiler, GCC 12.3 AArch64 cross compiler, POSIX/Linux APIs, OpenSSL candidate for SHA-256/HMAC after capability verification, Bash capability probes, ASan/UBSan/TSan.

---

## Status: DESIGN_BLOCKER — Task 4 PAUSED, Task 5 BLOCKED BY TASK 4

- **Task 4 entered DESIGN_BLOCKER on 2026-11-12.** The fault-batch close / freeze-publication sub-model is being replaced. Implementation stops here: **no fourth-round code patch**. The old Task 4 working tree stays as-is, and it is design-validation material only — never evidence, never a candidate for PASS.
- **Task 5 is BLOCKED BY TASK 4.** Do not start it, do not stub its symbols, do not "prepare" it in Task 4's files.
- **Task 3's compiler-only contract is unaffected** and its completed work stands; this round changes nothing inside the compiler/canonical/program contracts.
- The previous frozen SHA-256 `65F25A5A7CBC7E18F39ADDFA54CA0E8ACE5C6E67DEBFBA75480548EB4F215F5A` was invalidated by the Task 4 replacement. Codex reviewed the replacement on 2026-11-24 and re-froze the authoritative design as `9CE846BAA5EF14C941B07DC6C6D9848273B17FBAA268A1AE03DBE33808FECAA7`; the manifest and executable gate carry that value together.
- The revised design exists only as a **candidate** hash, reported in this round's review output; it is deliberately **not** written into the design document (self-referential), **not** recorded in the manifest, and **not** authoritative here. **Re-freezing is a separate change** after Codex approves the replacement (recompute hash + manifest + gate script + this status in one change).
- Evidence identifiers span **R1–R165**. R165 is new and `NOT_RUN`. R137 and R163 were **withdrawn from PASS**; R10/R11/R12 must be re-verified against the new baseline. All other PASS rows keep their evidence but were executed under the now-invalidated hash `65F25A5A…`.
- Root cause being fixed (do not re-litigate it in code): §6.3 closed the batch before Phase A (mutating `fault_batch_open`/`fault_batch_cutoff`/`fault_batch_digest`), Phase B was pinned to `registry → snapshot → state`, and §6.4 still required those three fields to be closed after success. The replacement deletes the duplicated runtime fields and makes `RecoverySlotState` + `FrozenRuntimeSnapshot` the single source of truth, with `state = FROZEN` as the only linearization point.

## Authoritative input and non-negotiable constraints

- Authoritative design (**frozen after the 2026-11-24 review**): `/mnt/e/Obsidian_warehouse/嵌入式/1-Project/RK3588工业安全监测/2026-11-02-DMA-BUF恢复与证明内核设计.md`
- Frozen SHA-256: `9CE846BAA5EF14C941B07DC6C6D9848273B17FBAA268A1AE03DBE33808FECAA7`. The document remains read-only for this repository: future implementation conflicts are reported as DESIGN_BLOCKERs, never resolved by editing the design.
- Implementation repository: `<repo>/rk3588`
- Existing branch at planning time: `codex/elf2-build-env`
- Existing modified/untracked files belong to the user. Never reset, delete, overwrite, or include them through `git add -A`.
- The design document is read-only for this repository. Implementation conflicts are reported as blockers; they are not silently resolved by changing the design.
- Each behavior uses strict RED → verify expected failure → minimal GREEN → full regression → refactor.
- For one design contract, three failed correction rounds are the limit. After the third failure, stop and report `DESIGN_BLOCKER` with the design clause, failing test, observed behavior, attempted fixes, and required design decision. Do not perform a fourth rebuild.
- R1–R165 are evidence identifiers. Preserve the R number in test names and the traceability matrix even when implementation order differs.
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
make recovery-cross-check          # AArch64 compile/link probe + Task 3 source-boundary scan; no board execution
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
cd <repo>/rk3588
git status --short --branch
```

Expected: the existing `.gitignore`, `aliyun_mqtt.c`, `encoder.c/.h`, interview/technical documents, SDK scripts, and tests remain untouched.

- [ ] **Step 2: Write the failing baseline and build-contract tests**

`tests/test_recovery_design_baseline.sh` must:

1. accept `RECOVERY_DESIGN_DOC`, defaulting to the authoritative path above;
2. verify the exact frozen SHA-256 from the manifest; any mismatch must fail until a new review updates the manifest and gate together;
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

`docs/recovery/recovery-traceability.md` must contain columns `ID | design clause | automated test | environment | status | evidence`. Initialize R1–R165 and §19.1–§19.21 as `NOT_RUN`/`UNVERIFIED`; do not mark anything PASS merely because a test file exists.

- [ ] **Step 6: Commit only owned files**

```bash
git -C <repo> add -- \
  rk3588/Makefile rk3588/mk/recovery.mk \
  rk3588/tests/test_recovery_design_baseline.sh \
  rk3588/tests/test_recovery_build.sh rk3588/tests/recovery \
  rk3588/docs/recovery \
  rk3588/docs/superpowers/plans/2026-11-09-dma-buf-recovery-kernel.md
git -C <repo> diff --cached --check
git -C <repo> commit -m "test: bootstrap recovery kernel verification"
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

### Task 3: Pin the digest implementation and the compiler-only process-exit contract

**Scope boundary (hard):** this task must compile, pass, and be reviewable **without a single fd**. If a step needs `commit_freeze`, `pidfd_open`, `/proc/<pid>/stat`, `poll`, `waitid`, the runtime registry, or a state transition, it belongs to Task 4 or Task 5 — do not implement it here. Task 3 defines *values* and *pure functions*; Task 4 creates fd-carrying bindings; Task 5 reads kernel facts.

**Files:**
- Create: `recovery/include/recovery/crypto.hpp`
- Create: `recovery/src/crypto_openssl.cc`
- Create: `recovery/include/recovery/program.hpp` — `ValidatedRecoveryProgram` **and** `FrozenRuntimeSnapshot` (with `std::vector<ProcessExitBindingDescriptor> process_exit_bindings`). The snapshot aggregate already references `FaultFact`/`OwnerExecutorBinding`/`EpochSnapshot`, so it belongs next to the program value types, not in `types.hpp`.
- Create: `recovery/include/recovery/compiler.hpp` — `RecoveryCompiler` with the four-argument production API and the private `compile_impl(..., CompilerAllocationBoundary&)` seam; declarations of `process_exit_bindings_are_legal` and `find_process_exit_binding`
- Create: `recovery/src/compiler.cc` — `derive_predicates` wiring, action closure, predicate/target exact matching, program construction and constructor-time transaction hash
- Modify: `recovery/include/recovery/types.hpp` — add `ProcessExitBindingSeq`, `ProcessExitBindingId`, `ProcessExitBindingDescriptor` **only** (data only; **no** fd member, **no** `OwnedProcessExitBinding`). Do **not** move `FaultFact`/`OwnerExecutorBinding`/`EpochSnapshot` here, and do **not** put `FrozenRuntimeSnapshot` here: `types.hpp` must stay free of any dependency on `program.hpp`, otherwise the two headers include each other.
- Modify: `recovery/include/recovery/canonical.hpp` — declare `canonical_write` for `RecoveryInstanceId`, `ProcessExitBindingId`, `ProcessExitBindingDescriptor`, and declare `canonical_write(const FrozenRuntimeSnapshot&)` from a **forward declaration** (`struct FrozenRuntimeSnapshot;`) rather than including `program.hpp`
- Modify: `recovery/src/canonical.cc` — include `program.hpp`, implement those overloads, and place `process_exit_bindings` between `owner_executor_bindings` and `epoch_snapshot` inside the snapshot encoding
- Create: `tests/recovery/test_process_exit_binding.cc` — R162 (structure five + missing one)
- Create: `tests/recovery/test_crypto_vectors.cc`
- Create: `tests/recovery/test_compiler.cc`
- Create: `tests/recovery/compile_fail/program_construct.cc`
- Create: `tests/recovery/compile_fail/program_mutate.cc`
- Create: `tests/recovery/compile_fail/program_move_assign.cc`
- Create: `tests/recovery/compile_fail/compiler_fifth_argument.cc`
- Create: `tests/test_recovery_task3_boundary.sh` — source-boundary scan for the compiler/canonical translation units (no fd symbols, no syscall headers); **not** a compile-fail test
- Create: `tests/probes/recovery_cross_compile_probe.cc`
- Modify: `mk/recovery.mk`
- Modify: `docs/recovery/recovery-traceability.md`

- [ ] Before implementation, add `recovery-cross-check` that compiles and links `canonical.cc`, `crypto_openssl.cc`, `compiler.cc`, `recovery_cross_compile_probe.cc`, and all public recovery headers with both host and the configured Buildroot AArch64 compiler/sysroot. The host binary must run; the target binary is compile/link only and must be reported `runtime NOT_RUN`. A digest-only probe is insufficient. If target linking fails, stop as an environment blocker; do not write a second hash algorithm.
- [ ] Write RED SHA-256 library vectors **and an independently precomputed fixed `canonical_digest` vector**. The canonical vector must compare a concrete canonical object directly with a literal 32-byte digest; `canonical_digest(x) == digest_bytes(canonical_bytes(x))` alone is self-consistency, not a golden vector.
- [ ] Define the process-exit descriptor value types and their canonical encoding **as data only**: `ProcessExitBindingId{recovery_id, freeze_seq, binding_seq}`, `ProcessExitBindingDescriptor{id, process}` (`process` is the full `ProcessIdentity`: pid **and** start_generation), `FrozenRuntimeSnapshot.process_exit_bindings`. The three new types go into `types.hpp`; `FrozenRuntimeSnapshot` stays in `program.hpp` (it already references `FaultFact`/`OwnerExecutorBinding`/`EpochSnapshot`), so `types.hpp` never has to include `program.hpp` and no cycle appears. `canonical_write(FrozenRuntimeSnapshot)` must place `process_exit_bindings` between `owner_executor_bindings` and `epoch_snapshot` and must cover no fd. No `OwnedProcessExitBinding` type exists in Task 3's headers.
- [ ] Implement `process_exit_bindings_are_legal(const FrozenRuntimeSnapshot&) noexcept` with exactly five structural checks: duplicate `ProcessIdentity`, reused `ProcessExitBindingId`, `id.recovery_id`/`id.freeze_seq` mismatch with the snapshot header, `binding_seq != vector index`, and `ProcessIdentity` **not strictly ascending by `(pid, start_generation)`**. The ordering check must be written as `if (i > 0 && !(v[i-1].process < v[i].process)) return false;` and must **not** use `ProcessExitBindingDescriptor::operator<=>` — that comparison looks at `id` first, and `binding_seq` is pinned to the index, so it accepts two descriptors whose `process` fields were swapped. Keep it allocation-free (O(n²) scan, no `std::set`).
- [ ] Wire `derive_predicates`: call `process_exit_bindings_are_legal()` once at entry (structural rejection) and `find_process_exit_binding()` per subject (missing rejection). **Both paths return `PredicateDerivationFailure{EXECUTION_TARGET_UNRESOLVED}`**; no new failure kind, and the missing case must not be folded into the legality function.
- [ ] Source-boundary check for the fd contract: add `tests/test_recovery_task3_boundary.sh` and wire it into `recovery-cross-check` (or a dedicated `recovery-task3-boundary` target invoked by `make recovery-cross-check`). The script scans the compiler and canonical translation units (`recovery/include/recovery/{compiler,canonical,types,program,crypto}.hpp`, `recovery/src/{compiler,canonical,crypto_openssl}.cc`, plus any header they include from `recovery/include/recovery/`) and fails if any of these appear: `OwnedFd`, `OwnedProcessExitBinding`, `pidfd_open`, `waitid`, `poll(`, `<sys/pidfd.h>`, `<sys/wait.h>`. On failure it must print the offending **file path and matching line** (e.g. `recovery/src/compiler.cc:42: pidfd_open(...)`), not just a count. This is a **source-text boundary check** — it proves "no fd semantics leak into Task 3's translation units"; it does **not** prove anything about overload resolution and must not be presented as compile-fail evidence. The "no `canonical_write` overload for the fd-carrying type" claim is proven by Task 4's compile-fail case.
- [ ] Implement SHA-256 behind `canonical_digest` with no algorithm selector in public APIs. Use the audited library, not a handwritten hash.
- [ ] Implement the pure four-argument production API only: `RecoveryCompiler::compile(snapshot, policy, registry, now)`. Public headers must expose no allocator/provider fifth argument. OOM injection uses private `compile_impl(..., CompilerAllocationBoundary&)`, reachable only through `RECOVERY_TESTING` friend access; cover every compiler allocation stage and R138 without a program mutator.
- [ ] Implement private program construction, deleted copy operations and move assignment, deterministic move construction through `CompileResult`, action closure failures, predicate/target exact matching, and constructor-time transaction hash exactly as §§7–8 and §15 require. Task 3 does **not** claim coordinator installation/seal state transitions from §9.
- [ ] Map and execute Task 3 rules individually:

| Rule | Required Task 3 test/evidence | Task 3 result required |
| --- | --- | --- |
| R1 | `program_construct.cc`: default and full-field construction both fail for private constructor | PASS |
| R2 | `empty_fault_batch_has_explicit_failure`; compile result has no default-success alternative | PASS |
| R3 | `program_mutate.cc` plus `program_move_assign.cc`: field mutation and whole-program move assignment fail compilation | PASS |
| R4 | `test_runtime_seal.cc::tampered_program_hash_is_rejected` | NOT_RUN until Task 4; never counted as Task 3 PASS |
| R5 | `test_runtime_seal.cc::different_compiled_object_is_rejected` | NOT_RUN until Task 4 |
| R6 | `test_runtime_seal.cc::changed_snapshot_policy_or_registry_is_rejected` | NOT_RUN until Task 4 |
| R7 | table-driven test for every `CompileFailure::Kind`; none yields a program/default `{}` | PASS |
| R8 | `compiler_fifth_argument.cc` and API compile probe: exactly four snapshot arguments; no global-policy overload | PASS |
| R9 | two compiles compare canonical bytes of faults/plans/coverage/predicates/expectations/DAG and transaction hash | PASS |
| R43 | `test_runtime_seal.cc::recomputed_hash_detects_each_field_tamper` | NOT_RUN until Task 4 |
| R44 | private full-field construction compile-fail in Task 3; object-identity/hash seal rejection in Task 4 | Task 3 partial only; overall NOT_RUN |
| R56 | private test seam makes derived predicate set missing/replaced before construction; compile returns `TARGET_PREDICATE_MISMATCH`, no program | PASS |
| R127 | table-driven closure test covers all five failure kinds plus recipe-index error; no empty-DAG/semantic exception path | PASS |
| R138 | private allocation boundary fails each compiler stage; each returns only `ALLOCATION_FAILED` and no program | PASS |
| R162 | `test_process_exit_binding.cc`: five structural cases (duplicate identity, reused id, header mismatch, `binding_seq != index`, non-ascending `process`) plus the missing-descriptor case. The ordering case **swaps only the `process` fields** while leaving `id`, `binding_seq`, and vector positions unchanged | PASS (all six return exactly `EXECUTION_TARGET_UNRESOLVED`) |

- [ ] Update traceability with the exact executed command and test name for each PASS. R4–R6/R43/R44 remain `NOT_RUN` until Task 4 runtime seal tests execute; `NOT_RUN`, `SKIP`, compilation-only existence, and target compile/link are never PASS.
- [ ] Run:

```bash
make recovery-test
make recovery-compile-fail-test
make recovery-sanitize-test
make recovery-cross-check          # also runs the Task 3 source-boundary scan
```

Expected: Task 3 required rows above are PASS (including R162); deferred seal rows remain explicitly NOT_RUN; deterministic hashes/programs and all forbidden construction/mutation/API probes fail for the intended reason. Task 3 must contain no fd, no syscall, and no state transition — that claim is evidenced by `tests/test_recovery_task3_boundary.sh` reporting zero matches, not by a compile-fail case (a compile-fail case for `OwnedProcessExitBinding` cannot exist yet, because the type is introduced in Task 4).

### Task 4: Fault batching, per-recovery ownership, freeze-time exit bindings (PAUSED — DESIGN_BLOCKER)

> **PAUSED (2026-11-12).** The fault-batch close / freeze-publication sub-model is being replaced (§6.3 / §6.4 / I35 / R165), the `FaultIdentity` issuance contract is being closed (§4.5 / I36 / R165 ⑦), and the **fault-ingress / replay sub-model has been replaced** after three non-converging rounds: the recovery kernel no longer deduplicates anything (§4.5 / §4.6 / I37 / I38 / R165 ⑧ / R13) — every successful `report_fault` is a new supervisory fact, and message idempotency lives in `TrustedFaultIngress` outside the kernel. The former `FaultReportToken` / `ReporterId` / `last_report_seq` design is **deleted**; do not reintroduce it. **Do not write a fourth-round patch against the current `runtime.cc`.** The files below already exist in the working tree: they are **design-validation material**, not evidence, and none of them may be cited as PASS. Resume only after Codex approves the replacement and the baseline is re-frozen.

**Files:**
- Modify: `recovery/include/recovery/runtime.hpp` — `RecoveryInstanceRuntime` keeps `std::vector<FaultFact> fault_batch` **only**; no `fault_batch_open` / `fault_batch_cutoff` / `fault_batch_digest`; `report_fault` takes `const FaultReport&` (fault content only — no identity, no token, no delivery id); `RecoverySlot` gains **no** delivery-side state at all (no `last_report_seq`, no reporter registry); `FaultIdentity` becomes private-constructor + `friend class RecoveryCoordinator`, with the test friend (`friend struct RecoveryTestAccess;`) and the type itself inside `#ifdef RECOVERY_TESTING`
- Modify: `recovery/src/runtime.cc` — add `prepare_fault_batch_close()` and the new `commit_freeze()` shape (design §6.3 / §6.4); delete every `close_fault_batch()` path
- Modify: `tests/recovery/test_fault_batch.cc`, `test_runtime_lifecycle.cc`, `test_fallback.cc`
- Modify: `tests/recovery/test_process_exit_freeze.cc` — target collection, before/after double-read bracketing, two-phase publication (R137, R163)
- Modify: `tests/recovery/test_process_exit_ownership.cc` — `OwnedFd` RAII, registry lifetime, fd baseline
- Create: `tests/recovery/test_fault_batch_freeze.cc` — **R165**: fault-batch single source of truth, identity issuance (⑦a–⑦g), "identical content is two facts" and the exception boundary (⑧)
- Keep: `tests/recovery/compile_fail/owned_process_exit_canonical.cc` — R163: prove there is no `canonical_write` overload for `OwnedProcessExitBinding`
- Modify: `tests/test_recovery_compile_fail.sh` — add a per-case expected-diagnostic assertion (each case declares the reason it must fail, e.g. an `EXPECT-DIAG:` header line in the `.cc` file), so "it failed to compile" alone stops being enough
- Modify: `mk/recovery.mk` — `runtime.cc` needs `<sys/pidfd.h>` for `pidfd_open` and reads `/proc/<pid>/stat` for `start_generation`; it must **not** need `<sys/wait.h>`, `poll`, or `waitid` (those belong to `proof.cc` in Task 5). Keep the pidfd include out of the compiler/canonical translation units.
- Modify: `docs/recovery/recovery-traceability.md`

- [ ] Write RED tests for the 12 transitions in §§4–13 beginning with `report_fault`, `begin_freeze`, `commit_freeze`, compile/fallback/seal, quiesce, commit, and reopen.
- [ ] First cover fault ordering and closed-batch behavior (R10–R14) **against the new model**: a fact may be appended only while the state is PREPARING/FREEZING; there is no `fault_batch_open` flag to consult. The kernel performs **no replay detection whatsoever** (§4.5 / I37): every successful `report_fault` issues a new `FaultIdentity`, so **identical content produces two increasing identities and `fault_seq_counter += 2`** — content equality never means "the same event", and there is no token, watermark or digest comparison to write. `FROZEN` / `COMPILING` / `SEALED` reject every call (there is no "already covered → audit only" branch: a fresh identity can never equal a frozen identity); `FAIL_CLOSED` only appends an audit record. Then fallback severity (R15–R19, R39–R41).
- [ ] Implement `RecoverySlot` boot-global state and the single optional `RecoveryInstanceRuntime`; publish a fully prepared runtime atomically and destroy it only with `active.reset()` (R64, R70, R78, R88–R92, R102, R103).
- [ ] Close the `FaultIdentity` issuance contract (design §4.5 / I36 / I37, before anything else): `report_fault` takes `const FaultReport&` — fault content only, identity **no**, token **no**, delivery id **no**, so `boot_id` / `fault_seq` are not expressible by a caller. `FaultIdentity` gets a **private constructor** with `friend class RecoveryCoordinator`; the `RecoveryTestAccess` friend declaration and the type itself sit inside `#ifdef RECOVERY_TESTING`. `issue_fault_identity_locked()` is the only production construction site; the counter is committed **once**, after the runtime is published / the fact is appended, so any failure leaves counter and batch untouched and enters FAIL_CLOSED. Rejected branches consume **no** sequence number, and there is no "retry" concept to consume one either. `report_fault` is `noexcept` end to end: lock-construction failure → watchdog reboot, allocation failure → FAIL_CLOSED, never `std::terminate`. Write four compile-fail cases with exact diagnostics: `report_fault(FaultFact)`, `report_fault(FaultReport, FaultIdentity)`, an out-of-class `FaultIdentity id{boot, 1};` (private-constructor diagnostic), and one proving `RecoveryTestAccess` is unreachable in a production build (the type name does not exist, so it can neither access private members nor act as a friend). Keep the type order `FaultKind → FaultSubject → FaultEvidence → FaultReport → FaultIdentity → FaultFact` (no forward declarations for by-value members).
- [ ] Implement `prepare_fault_batch_close()` (§6.4) as a **pure local** step: reject an empty batch, capture `cutoff = slot_.fault_seq_counter` once at Phase A entry, build the canonical batch from `active.fault_batch`, verify boot match / `fault_seq <= cutoff` / unique `FaultIdentity` / strict canonical order / **last sequence number == cutoff**, compute the local digest. It must not write a single `slot_.active` field, and on any failure it returns `nullopt` (caller FAIL_CLOSED) with `active.fault_batch` byte-identical.
- [ ] Own the fd side of the freeze contract: `OwnedProcessExitBinding{ProcessExitBindingDescriptor descriptor; OwnedFd pidfd;}` and `RecoveryInstanceRuntime.process_exit_binding_registry` (`std::map<ProcessExitBindingId, OwnedProcessExitBinding>`). Raw fd never enters `FrozenRuntimeSnapshot`, any digest, or any header that the compiler includes.
- [ ] Add the compile-fail proof that the fd-carrying type cannot be canonicalized (**R163 / Task 4** — the type does not exist in Task 3, so this case cannot live there): `tests/recovery/compile_fail/owned_process_exit_canonical.cc` obtains a real `OwnedProcessExitBinding` (constructed in place, or fetched from `process_exit_binding_registry`) and calls `canonical_write(w, owned)`. The harness must fail the build **and** assert the diagnostic:
  - expected reason: `no matching function for call to 'canonical_write'` (GCC and Clang word it the same way for a missing overload);
  - a missing `#include`, an unknown/incomplete type, a typo, `-Werror` noise from unrelated files, or any other syntax error is a **harness FAIL**, never a PASS — otherwise the case degrades into "write something broken and call it proof";
  - record the exact assertion command in traceability next to the R163 row.
- [ ] Build the target set exactly once: `ProcessIdentity`-typed `execution_targets` of the frozen ledger prefix ∪ the `WORKER_PROCESS_EXIT` subjects **of the local canonical batch from `prepare_fault_batch_close()`** (never re-read `active.fault_batch` here); deduplicate and order by `(pid, start_generation)` via `std::set<ProcessIdentity>`. `binding_seq` is that index and nothing else.
- [ ] Implement `bind_one_process_exit` as **before/after double-read bracketing**: `read_start_generation(before)` → compare with the expected `ProcessIdentity.start_generation` → `pidfd_open(pid, PIDFD_NONBLOCK)` → `read_start_generation(after)` → compare again. The generation is read **exactly twice**; there is no third `/proc` read. Accept only if both reads succeed and both equal the expected value; return an explicit failure reason for each of the five rejection points (`READ_BEFORE_FAILED`, `GENERATION_MISMATCH_BEFORE`, `PIDFD_OPEN_FAILED`, `READ_AFTER_FAILED`, `GENERATION_MISMATCH_AFTER`); a single failure aborts the whole freeze (never "skip this target"). The bracketing only bounds the `pidfd_open` window — it does not prove that the fd refers to the identity that was read; that is the `pidfd` semantic question tracked as a board item (§19 item 10, currently UNVERIFIED).
- [ ] Implement `commit_freeze` as two phases over `PreparedFaultBatchClose` / `PreparedProcessExitBindings{descriptors, registry}` / `PreparedFreezeCommit{snapshot, registry}`:
  - phase A builds everything on locals (fault-batch close values, target set, bindings, snapshot deep copy, `process_exit_bindings_are_legal`, `descriptors_registry_agree`, `snapshot_digest`) with **zero** `slot_.active` side effects — in particular `active.fault_batch` is only read;
  - phase B performs only nothrow assignments in the order `registry → snapshot → state = FROZEN`, guarded by `static_assert(std::is_nothrow_move_assignable_v<...>)` for the registry map and `FrozenRuntimeSnapshot` (and therefore its `std::optional`); `state` is the single linearization point where "batch closed", "snapshot visible" and "registry visible" become true together;
  - any failure in phase A → `fail_closed_locked(...)`, both containers **and `active.fault_batch`** keep their previous values byte-for-byte, and locally opened fds are closed by `OwnedFd` destructors.
- [ ] Implement `descriptors_registry_agree()` and `process_exit_bindings_are_legal()` reuse: descriptor without a valid fd, fd without a descriptor, key ≠ `descriptor.id`, or different `process` all reject the commit (R163).
- [ ] Re-entering `commit_freeze` after publication must FAIL_CLOSED, not overwrite (`current_snapshot.has_value() || !registry.empty()`).
- [ ] Add OOM/failure-provider cases at **every** phase-A checkpoint (owner ack, prefix digest, fault-batch not freezeable, empty batch, each of the five bind failures, legality, digest) and assert: `state == FAIL_CLOSED`, `active.fault_batch` **and `fault_seq_counter`** byte-identical, no `current_snapshot`, empty registry, `/proc/self/fd` back to baseline. That is R137's extended scope and R165's matrix.
- [ ] Implement `finish_compile` and `seal_validated_program` object-identity/digest/hash revalidation and execute R4–R6/R43/R44 tests named in Task 3's mapping table. Only this task may change those traceability rows from NOT_RUN to PASS. R44's object-identity/hash half is completed here.
- [ ] Run filtered tests after every state transition, then the full host/sanitizer suite. Assert `/proc/self/fd` returns to baseline on every failure path.

### Task 5: Implement proof matching, trusted process-exit observation, and the exact proof barrier

> **IMPLEMENTED (2026-11-22), RE-FROZEN (2026-11-24).** Task 5 consumes the frozen snapshot produced by the rebuilt Task 4. The external design baseline passed review and is frozen at `9CE846BA…FECAA7`. The implementation keeps `observe_process_exit_trusted` and proof matching in `proof.cc`/`proof.hpp`, outside Task 4's runtime translation unit.

**Files:**
- Create: `recovery/include/recovery/proof.hpp`
- Create: `recovery/src/proof.cc`
- Create: `tests/recovery/test_proof_matching.cc`
- Create: `tests/recovery/test_proof_barrier.cc`
- Create: `tests/recovery/test_process_exit_observation.cc` — observation failure semantics and R46/R164
- Modify: `mk/recovery.mk` — `proof.cc` is the only translation unit that gets `<sys/wait.h>`/`<poll.h>` (`poll`, `waitid`); keep them out of `runtime.cc` and out of the compiler units
- Modify: `docs/recovery/recovery-traceability.md`

- [x] Write RED cases for the implemented R20–R24, R45–R47, R59, and R164 paths, including cross-recovery replay, duplicate predicate key, wrong variant alternative, registry identity/revocation, mismatched pidfd generation, and trusted observation failures. R48/R49/R50/R57/R58/R63 remain separately tracked where their device-specific or capability evidence needs additional tests.
- [x] Define `TrustedProcessExitObservation{binding_id, process, wait_code, wait_errno, exited, spawn_generation}` and `observe_process_exit_trusted(const OwnedProcessExitBinding&) noexcept`. The signature takes **only** the trusted owned binding: no `wait_code`, no `wait_errno`, no binding id from the proof record, and no overload that accepts them.
- [x] Implement the observation body over `poll(pidfd, POLLIN, 0)` + `waitid(P_PIDFD, fd, &si, WEXITED | WNOHANG | WNOWAIT)` with the specified closed-fd, poll-error, not-exited, normal-exit, signaled-exit, and declaration-mismatch semantics. Never return `true` by copying a declared value.
- [x] Implement the `PROCESS_EXIT_CONFIRMED` branch of `execution_proof_matches` in this order: predicate has `expected_process_binding` → `proof.binding_id` equals it → registry lookup hits → **map key equals `owned.descriptor.id`** → `owned.descriptor.process` equals the predicate's expected `ProcessIdentity` → `owned.pidfd.valid()` → observation is valid and `exited` → declared `wait_code`/`wait_errno`**and** `spawn_generation` equal the observed values → `terminated_confirmed`.
- [x] Add R46 + R164 coverage for wrong binding/registry identity, closed fd, not-yet-exited, declared wait/generation mismatch, poll errno, normal exit, and signal exit. Include the adversarial case "proof declares a fully successful exit while the trusted observation says otherwise".
- [x] Implement `VerificationContext` from a copied registry snapshot, `ProofIndex` keyed by `PredicateKey`, proof-record canonical hashing, and exact-set barrier comparison.
- [ ] Keep DeviceTarget proofs that depend on §19 capabilities rejected or `BOARD_RESTART_REQUIRED` until target evidence exists.
- [ ] Run `make recovery-test recovery-sanitize-test`; record only observed PASS results. `WNOWAIT` leaves the child waitable — if the test harness must reap, it does so after asserting observation, and that ordering is recorded in the evidence.

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
- [ ] Cover R108 here: allocation failure while building authorization eligible/accepted-prerequisite sets returns `INTERNAL_FAILURE`, enters fail-closed as specified, and leaves the journal byte-for-byte unchanged.
- [ ] Cover R109 here: prerequisite-set allocation failure in `validate_receipt_locked` returns `ReceiptValidateError{ALLOCATION_FAILED}` without consuming the lease or modifying `ReceiptIndex`.
- [ ] Cover R110 here: `poll_authorizations` reserve/push failure returns `PollError{ALLOCATION_FAILED}` with journal and `RequestIndex` unchanged.
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
- [ ] Test `waitid(P_PIDFD, WEXITED|WNOHANG|WNOWAIT)` availability, `poll(POLLIN)` firing on an exited child's pidfd, `si_code`→`WaitCode` mapping, and whether `WNOWAIT` leaves the child waitable plus who eventually reaps it — these are exactly the preconditions of `observe_process_exit_trusted`. Then pid reuse/starttime, SO_PEERCRED/user namespace, CSPRNG startup behavior, SEQPACKET framing/MSG_TRUNC/buffer limits, poll/HUP/ERR shutdown latency, fd leak paths, OpenSSL HMAC/constant-time function, RGA/MPP/RKNN semantics, V4L2 orphaned buffers, reset evidence, and watchdog terminal observability.
- [ ] Mark only observed capabilities `VERIFIED`; use `UNSUPPORTED` with the designed fallback where appropriate. A skipped or unavailable probe remains `UNVERIFIED`.
- [ ] Re-run the dependent host tests with the capability manifest and ensure no unverified DeviceTarget emits success proof.

### Task 11: Close R1–R165 and prepare integration, without changing the architecture

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

- [ ] Verify every R1–R165 row has a test name, environment, result, and evidence reference. `NOT_RUN`, `UNVERIFIED`, or `SKIP` is not PASS.
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
6. deviations from the authoritative design (normally `none`);
7. correction-attempt count for each unresolved behavior;
8. `git status --short` proving unrelated existing changes were preserved.

Do not start the next task until the current report has been reviewed.
