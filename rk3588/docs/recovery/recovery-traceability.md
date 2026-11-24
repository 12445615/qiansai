# Recovery Traceability

Scope: R1–**R165** and §19.1–§19.21. The design input is **no longer frozen**: Task 4 entered `DESIGN_BLOCKER` and the previous frozen SHA-256 `65F25A5A7CBC7E18F39ADDFA54CA0E8ACE5C6E67DEBFBA75480548EB4F215F5A` is **INVALIDATED**; see `docs/recovery/recovery-design-baseline.md` (`status = INVALIDATED / PENDING_REVIEW`). The revised design exists only as a candidate hash reported in this round's review output; it is deliberately not written into the design document, not recorded in the manifest, and must not be treated as a baseline.

Status of the work itself: **Task 4 and Task 5 candidate implementations are complete, with the baseline still INVALIDATED / PENDING_REVIEW**. Task 5 now consumes the candidate frozen snapshot contract; its host proof/observation evidence is deliberately not claimed as a frozen-baseline PASS. The fault-batch close fact is now "state + `FrozenRuntimeSnapshot`", and the implementation uses `FaultReport` without caller identity, commits `fault_seq_counter` only after acceptance, rejects an empty batch at freeze, and performs no replay deduplication in the kernel. Byte-identical content is two independent facts with increasing identities; message idempotency remains outside the kernel in `TrustedFaultIngress`. `waitid(P_PIDFD)`/pidfd semantics on the RK3588 rootfs remain a separate §19 capability item and are not inferred from host evidence.

**Every remaining `PASS` row below was executed against the now-invalidated frozen hash `65F25A5A…`.** The evidence is kept for regression comparison only; it does not certify the candidate design and must not be quoted as current evidence. Any row re-executed later must record the new hash once a re-freeze happens.

| ID | design clause | automated test | environment | status | evidence |
| --- | --- | --- | --- | --- | --- |
| R1 | §18 R1 | `compile_fail/program_construct.cc` (default + full-field construction) | host | PASS | `make recovery-compile-fail-test`: intended private/no-matching-constructor diagnostic |
| R2 | §18 R2 | `R2_R7_empty_fault_is_explicit_failure` + non-default-constructible `CompileResult` assertion | host | PASS | `make -B recovery-test`: explicit `EMPTY_FAULT_BATCH`, no default success |
| R3 | §18 R3 | `compile_fail/program_mutate.cc`, `compile_fail/program_move_assign.cc` | host | PASS | `make recovery-compile-fail-test`: accessor mutation and whole-program move assignment rejected |
| R4 | §18 R4 | `R4_R43_each_program_component_tamper_is_rejected_by_runtime_seal` | host | PASS | `make -B recovery-test`: stale hash plus fault/plan/predicate/DAG tamper each enters FAIL_CLOSED |
| R5 | §18 R5 | `R4_R6_R44_runtime_seal_revalidates_identity_and_frozen_inputs` | host | PASS | `make -B recovery-test`: separately compiled but byte-identical program has different object identity and seal enters FAIL_CLOSED |
| R6 | §18 R6 | `R6_finish_compile_rejects_changed_frozen_policy`, `R6_policy_contents_mutation_is_rejected_at_seal` | host | PASS | `make -B recovery-test`: frozen digest change prevents install; contents mutation prevents seal |
| R7 | §18 R7 | `R7_every_compile_failure_kind_is_an_explicit_error_alternative` | host | PASS | `make -B recovery-test`: all eight failure kinds represented only as `CompileFailure` |
| R8 | §18 R8 | `compile_fail/compiler_fifth_argument.cc` + cross compile probe | host + AArch64 compile/link | PASS | `make recovery-compile-fail-test recovery-cross-check`: fifth argument rejected; four-argument API compiles/links |
| R9 | §18 R9 | `R9_same_inputs_compile_byte_identically`, `R9_snapshot_canonical_order_pins_process_bindings_between_owner_and_epoch` | host | PASS | `make -B recovery-test`: canonical component bytes and transaction hash equal; snapshot field order pinned |
| R10 | §18 R10 | `R10_R14_fault_batch_is_canonical_and_closed`, `R165_identical_reports_are_two_new_kernel_facts` | host | CANDIDATE PASS / pending re-freeze | `make -B recovery-test`: two byte-identical reports append two identities; successful freeze asserts exact snapshot batch/cutoff/digest; no kernel deduplication |
| R11 | §18 R11 | `R11_report_fault_and_commit_freeze_are_mutex_linearized` | host + ASan/UBSan | CANDIDATE PASS / pending re-freeze | `make -B recovery-test recovery-sanitize-test`: mutex linearization and frozen snapshot batch/cutoff/digest match |
| R12 | §18 R12 | `R165_frozen_snapshot_is_unchanged_when_identical_report_is_rejected` | host + ASan/UBSan | CANDIDATE PASS / pending re-freeze | `make -B recovery-test recovery-sanitize-test`: identical post-FROZEN report fails closed and leaves all three published batch fields unchanged |
| R13 | §18 R13 | `R165_identical_reports_are_two_new_kernel_facts`, `R165_failed_append_preserves_first_fact_and_counter`, `recovery-task4-boundary` | host + ASan/UBSan | CANDIDATE PASS / pending re-freeze | Same commands plus boundary scan: counter advances only for accepted facts; failed append preserves batch/counter; deleted replay-token names are absent |
| R14 | §18 R14 | `R14_new_scope_fault_after_seal_fails_closed` | host + ASan/UBSan | PASS | fresh full gate: a new resource scope after SEALED enters FAIL_CLOSED |
| R15 | §18 R15 | `R19_R41_unknown_without_action_is_explicit_fail_closed` | host + ASan/UBSan | PASS | single UNKNOWN derives FAIL_CLOSED, empty plans, no fallback action |
| R16 | §18 R16 | `R16_capability_fault_requires_board_restart_and_has_no_dag_input` | host + ASan/UBSan | PASS | single capability fault derives BOARD_RESTART_REQUIRED with no online plan |
| R17 | §18 R17 | `R17_known_plus_unknown_does_not_recover_the_known_subset` | host + ASan/UBSan | PASS | mixed known+UNKNOWN returns empty plans and FAIL_CLOSED |
| R18 | §18 R18 | `R17_R18_R39_mixed_unrecoverable_chooses_board_and_no_online_plan` | host + ASan/UBSan | PASS | BOARD dominates parent and covered faults; plans remain empty |
| R19 | §18 R19 | `R19_fallback_result_cannot_be_sealed_as_a_program` | host + ASan/UBSan | PASS | fallback installs no program; seal attempt rejects and enters FAIL_CLOSED |
| R20 | §18 R20 | `R46_declared_success_is_rejected_when_trusted_identity_differs` | host | CANDIDATE PASS / pending re-freeze | `make recovery-test`: a proof with a different recovery id is rejected before trusted binding use |
| R21 | §18 R21 | `R20_R21_R22_barrier_requires_exact_typed_predicate_set` | host | CANDIDATE PASS / pending re-freeze | `make recovery-test`: `ProofIndex` accepts one record per typed key; the second insertion is rejected and exact-set completion remains true |
| R22 | §18 R22 | `R20_R21_R22_barrier_requires_exact_typed_predicate_set`, `R23_R24_registry_identity_and_evidence_validity_are_rechecked` | host | CANDIDATE PASS / pending re-freeze | `make recovery-test`: execution/device variant-key mismatches and typed result identity mismatches are rejected |
| R23 | §18 R23 | `R23_R24_registry_identity_and_evidence_validity_are_rechecked` | host | CANDIDATE PASS / pending re-freeze | `make recovery-test`: registry version mismatch is rejected even with otherwise matching evidence |
| R24 | §18 R24 | `R23_R24_registry_identity_and_evidence_validity_are_rechecked` | host | CANDIDATE PASS / pending re-freeze | `make recovery-test`: revoked evidence is rejected |
| R25 | §18 R25 | TBD | host | NOT_RUN | — |
| R26 | §18 R26 | TBD | host | NOT_RUN | — |
| R27 | §18 R27 | TBD | host | NOT_RUN | — |
| R28 | §18 R28 | TBD | host | NOT_RUN | — |
| R29 | §18 R29 | TBD | host | NOT_RUN | — |
| R30 | §18 R30 | TBD | host | NOT_RUN | — |
| R31 | §18 R31 | TBD | host | NOT_RUN | — |
| R32 | §18 R32 | TBD | host | NOT_RUN | — |
| R33 | §18 R33 | TBD | host | NOT_RUN | — |
| R34 | §18 R34 | TBD | host | NOT_RUN | — |
| R35 | §18 R35 | TBD | host | NOT_RUN | — |
| R36 | §18 R36 | TBD | host | NOT_RUN | — |
| R37 | §18 R37 | TBD | host | NOT_RUN | — |
| R38 | §18 R38 | TBD | host | NOT_RUN | — |
| R39 | §18 R39 | `R39_deleting_streamoff_or_reqbufs_zero_is_rejected_at_seal` | host + ASan/UBSan | PASS | each required V4L2 teardown operation is independently erased under the private test seam; stale canonical transaction hash is rejected at seal |
| R40 | §18 R40 | `R40_missing_owner_ack_fails_closed_without_snapshot`, `R40_owner_snapshot_ack_exact_validation_and_complete_set` | host + ASan/UBSan | PASS | missing ACK rejects commit; header, owner, exact grant/lease set, digest, duplicate paths reject; complete set publishes canonical ACK digest |
| R41 | §18 R41 | `R41_failed_derivation_cannot_be_disguised_as_compile_success` | host + ASan/UBSan | PASS | public compile returns the CompileFailure alternative; without an executable fallback action finish_compile installs no program and immediately enters FAIL_CLOSED |
| R42 | §18 R42 | `R42_canonical_nested_lengths_prevent_concatenation_collision` | host g++ / C++20; ASan+UBSan | PASS | `make recovery-test recovery-compile-fail-test recovery-sanitize-test`; `tests/recovery/test_canonical.cc` |
| R43 | §18 R43 | `R4_R43_each_program_component_tamper_is_rejected_by_runtime_seal` | host | PASS | `make -B recovery-test`: hash/fault/plan/predicate/DAG mutations retain stale hash and are rejected at seal |
| R44 | §18 R44 | `compile_fail/program_construct.cc`, `R4_R6_R44_runtime_seal_revalidates_identity_and_frozen_inputs` | host | PASS | `make recovery-compile-fail-test recovery-test`: direct construction rejected and different compiled object identity rejected |
| R45 | §18 R45 | `R45_thread_join_result_identity_is_checked` | host | CANDIDATE PASS / pending re-freeze | `make recovery-test`: typed `ThreadJoinResult.thread` mismatch returns false |
| R46 | §18 R46 | `R46_successful_process_exit_requires_trusted_observation`, `R46_declared_success_is_rejected_when_trusted_identity_differs`, `R46_registry_key_must_equal_owned_descriptor_id`, `R46_declared_wait_and_generation_must_match_observation` | host | CANDIDATE PASS / pending re-freeze | `make recovery-test`: trusted owned binding, descriptor/map-key equality, pidfd observation, wait fields, generation, and cross-recovery checks all gate success |
| R47 | §18 R47 | `R164_not_yet_exited_is_unknown_and_not_terminated`, `R46_declared_success_is_rejected_when_trusted_identity_differs` | host | CANDIDATE PASS / pending re-freeze | `make recovery-test`: UNKNOWN/not-exited and invalid trusted observation cannot satisfy a declared successful exit |
| R48 | §18 R48 | `R48_R49_device_result_identity_and_reset_generation_are_checked` | host | CANDIDATE PASS / pending re-freeze | `make recovery-test`: mismatched device context in a teardown result is rejected |
| R49 | §18 R49 | `R48_R49_device_result_identity_and_reset_generation_are_checked` | host | CANDIDATE PASS / pending re-freeze | `make recovery-test`: non-increasing hardware reset generation is rejected |
| R50 | §18 R50 | `device_predicate_is_derived_from_ledger_and_registry_evidence`, `R23_R24_registry_identity_and_evidence_validity_are_rechecked` | host | CANDIDATE PASS / pending re-freeze | `make recovery-test`: capability expectation comes from registry evidence and matcher rechecks the immutable registry contents |
| R48 | §18 R48 | TBD | host | NOT_RUN | — |
| R49 | §18 R49 | TBD | host | NOT_RUN | — |
| R50 | §18 R50 | TBD | host | NOT_RUN | — |
| R51 | §18 R51 | TBD | host | NOT_RUN | — |
| R52 | §18 R52 | `permit_move_assign.cc` deleted move-assignment rejection | host g++ / C++20 | PASS | `make recovery-compile-fail-test`; `build/host/recovery/compile_fail/permit_move_assign.cc.log` |
| R53 | §18 R53 | TBD | host | NOT_RUN | — |
| R54 | §18 R54 | TBD | host | NOT_RUN | — |
| R55 | §18 R55 | TBD | host | NOT_RUN | — |
| R56 | §18 R56 | `R56_missing_or_replaced_predicate_is_rejected` | host | PASS | `make -B recovery-test`: private test seam removes/replaces derived predicate; both return `TARGET_PREDICATE_MISMATCH` |
| R57 | §18 R57 | `device_fault_target_outside_ledger_closure_fails_explicitly` | host | CANDIDATE PASS / pending re-freeze | `make recovery-test`: device target outside the ledger closure returns `TARGET_NOT_IN_CLOSURE` |
| R58 | §18 R58 | `R58_registry_evidence_failure_kinds_are_explicit`, `device_predicate_is_derived_from_ledger_and_registry_evidence` | host | CANDIDATE PASS / pending re-freeze | `make recovery-test`: missing/ambiguous/revoked/expired evidence and successful registry-derived expectation are explicit |
| R59 | §18 R59 | `R59_typed_predicate_keys_do_not_interchange`; `cross_type_predicate.cc` | host g++ / C++20; ASan+UBSan | CANDIDATE PASS / pending re-freeze | `make recovery-test recovery-compile-fail-test recovery-sanitize-test`; typed `PredicateKey` alternatives do not collide |
| R60 | §18 R60 | TBD | host | NOT_RUN | — |
| R61 | §18 R61 | TBD | host | NOT_RUN | — |
| R62 | §18 R62 | TBD | host | NOT_RUN | — |
| R63 | §18 R63 | TBD | host | NOT_RUN | — |
| R64 | §18 R64 | TBD | host | NOT_RUN | — |
| R65 | §18 R65 | TBD | host | NOT_RUN | — |
| R66 | §18 R66 | TBD | host | NOT_RUN | — |
| R67 | §18 R67 | TBD | host | NOT_RUN | — |
| R68 | §18 R68 | TBD | host | NOT_RUN | — |
| R69 | §18 R69 | TBD | host | NOT_RUN | — |
| R70 | §18 R70 | TBD | host | NOT_RUN | — |
| R71 | §18 R71 | TBD | host | NOT_RUN | — |
| R72 | §18 R72 | TBD | host | NOT_RUN | — |
| R73 | §18 R73 | TBD | host | NOT_RUN | — |
| R74 | §18 R74 | TBD | host | NOT_RUN | — |
| R75 | §18 R75 | Preliminary type-level coverage: `R75_permit_binding_distinguishes_recovery_instances`; production `lease_matches` not implemented | host g++ / C++20; ASan+UBSan | NOT_RUN | `tests/recovery/test_types.cc`; production cross-recovery rejection remains unverified |
| R76 | §18 R76 | Preliminary type-level coverage: `R76_permit_move_and_consume_invalidate_handle`; production `consume_locked` not implemented | host g++ / C++20; ASan+UBSan | NOT_RUN | `tests/recovery/test_types.cc`; production consume path remains unverified |
| R77 | §18 R77 | TBD | host | NOT_RUN | — |
| R78 | §18 R78 | TBD | host | NOT_RUN | — |
| R79 | §18 R79 | TBD | host | NOT_RUN | — |
| R80 | §18 R80 | TBD | host | NOT_RUN | — |
| R81 | §18 R81 | TBD | host | NOT_RUN | — |
| R82 | §18 R82 | TBD | host | NOT_RUN | — |
| R83 | §18 R83 | TBD | host | NOT_RUN | — |
| R84 | §18 R84 | TBD | host | NOT_RUN | — |
| R85 | §18 R85 | TBD | host | NOT_RUN | — |
| R86 | §18 R86 | TBD | host | NOT_RUN | — |
| R87 | §18 R87 | TBD | host | NOT_RUN | — |
| R88 | §18 R88 | TBD | host | NOT_RUN | — |
| R89 | §18 R89 | TBD | host | NOT_RUN | — |
| R90 | §18 R90 | TBD | host | NOT_RUN | — |
| R91 | §18 R91 | TBD | host | NOT_RUN | — |
| R92 | §18 R92 | TBD | host | NOT_RUN | — |
| R93 | §18 R93 | TBD | host | NOT_RUN | — |
| R94 | §18 R94 | TBD | host | NOT_RUN | — |
| R95 | §18 R95 | TBD | host | NOT_RUN | — |
| R96 | §18 R96 | TBD | host | NOT_RUN | — |
| R97 | §18 R97 | TBD | host | NOT_RUN | — |
| R98 | §18 R98 | TBD | host | NOT_RUN | — |
| R99 | §18 R99 | TBD | host | NOT_RUN | — |
| R100 | §18 R100 | TBD | host | NOT_RUN | — |
| R101 | §18 R101 | TBD | host | NOT_RUN | — |
| R102 | §18 R102 | TBD | host | NOT_RUN | — |
| R103 | §18 R103 | TBD | host | NOT_RUN | — |
| R104 | §18 R104 | TBD | host | NOT_RUN | — |
| R105 | §18 R105 | TBD | host | NOT_RUN | — |
| R106 | §18 R106 | TBD | host | NOT_RUN | — |
| R107 | §18 R107 | TBD | host | NOT_RUN | — |
| R108 | §18 R108 | TBD | host | NOT_RUN | — |
| R109 | §18 R109 | TBD | host | NOT_RUN | — |
| R110 | §18 R110 | TBD | host | NOT_RUN | — |
| R111 | §18 R111 | TBD | host | NOT_RUN | — |
| R112 | §18 R112 | TBD | host | NOT_RUN | — |
| R113 | §18 R113 | TBD | host | NOT_RUN | — |
| R114 | §18 R114 | TBD | host | NOT_RUN | — |
| R115 | §18 R115 | TBD | host | NOT_RUN | — |
| R116 | §18 R116 | TBD | host | NOT_RUN | — |
| R117 | §18 R117 | TBD | host | NOT_RUN | — |
| R118 | §18 R118 | TBD | host | NOT_RUN | — |
| R119 | §18 R119 | TBD | host | NOT_RUN | — |
| R120 | §18 R120 | TBD | host | NOT_RUN | — |
| R121 | §18 R121 | TBD | host | NOT_RUN | — |
| R122 | §18 R122 | TBD | host | NOT_RUN | — |
| R123 | §18 R123 | TBD | host | NOT_RUN | — |
| R124 | §18 R124 | TBD | host | NOT_RUN | — |
| R125 | §18 R125 | TBD | host | NOT_RUN | — |
| R126 | §18 R126 | `R126_executor_principal_has_only_process_and_coordinator` | host g++ / C++20; ASan+UBSan | PASS | `make recovery-test recovery-sanitize-test`; `tests/recovery/test_types.cc` |
| R127 | §18 R127 | six `R127_*_is_explicit_closure_failure` tests | host | PASS | `make -B recovery-test`: five closure failure kinds plus recipe-index error are explicit, no empty-DAG fallback |
| R128 | §18 R128 | TBD | host | NOT_RUN | — |
| R129 | §18 R129 | TBD | host | NOT_RUN | — |
| R130 | §18 R130 | TBD | host | NOT_RUN | — |
| R131 | §18 R131 | TBD | host | NOT_RUN | — |
| R132 | §18 R132 | TBD | host | NOT_RUN | — |
| R133 | §18 R133 | TBD | host | NOT_RUN | — |
| R134 | §18 R134 | TBD | host | NOT_RUN | — |
| R135 | §18 R135 | TBD | host | NOT_RUN | — |
| R136 | §18 R136 | TBD | host | NOT_RUN | — |
| R137 | §18 R137 | `R165_phase_a_failures_preserve_batch_counter_and_publication`, `process_exit_binding_five_failures_fail_closed_without_publication`, `R137_failure_after_pidfd_open_closes_local_fd_without_publication` | host + ASan/UBSan | CANDIDATE PASS / pending re-freeze | `make -B recovery-test recovery-sanitize-test`: every injected Phase A failure preserves batch/counter, publishes no snapshot/registry, and closes local fds |
| R138 | §18 R138 | `R138_all_compile_allocation_stages_fail_explicitly` | host | PASS | `make -B recovery-test`: four private allocation stages each return only `ALLOCATION_FAILED`, no program |
| R139 | §18 R139 | TBD | host | NOT_RUN | — |
| R140 | §18 R140 | TBD | host | NOT_RUN | — |
| R141 | §18 R141 | TBD | host | NOT_RUN | — |
| R142 | §18 R142 | TBD | host | NOT_RUN | — |
| R143 | §18 R143 | TBD | host | NOT_RUN | — |
| R144 | §18 R144 | TBD | host | NOT_RUN | — |
| R145 | §18 R145 | TBD | host | NOT_RUN | — |
| R146 | §18 R146 | TBD | host | NOT_RUN | — |
| R147 | §18 R147 | TBD | host | NOT_RUN | — |
| R148 | §18 R148 | TBD | host | NOT_RUN | — |
| R149 | §18 R149 | TBD | host | NOT_RUN | — |
| R150 | §18 R150 | TBD | host | NOT_RUN | — |
| R151 | §18 R151 | TBD | host | NOT_RUN | — |
| R152 | §18 R152 | TBD | host | NOT_RUN | — |
| R153 | §18 R153 | TBD | host | NOT_RUN | — |
| R154 | §18 R154 | TBD | host | NOT_RUN | — |
| R155 | §18 R155 | TBD | host | NOT_RUN | — |
| R156 | §18 R156 | TBD | host | NOT_RUN | — |
| R157 | §18 R157 | TBD | host | NOT_RUN | — |
| R158 | §18 R158 | TBD | host | NOT_RUN | — |
| R159 | §18 R159 | TBD | host | NOT_RUN | — |
| R160 | §18 R160 | TBD | host | NOT_RUN | — |
| R161 | §18 R161 | TBD | host | NOT_RUN | — |
| R162 | §18 R162 | six `R162_*_is_unresolved` tests in `test_process_exit_binding.cc` through public four-argument `RecoveryCompiler::compile` | host | PASS | `make -B recovery-test`: valid non-binding prerequisites; all five structural cases plus missing descriptor return outer `PREDICATE_DERIVATION_FAILED` with inner `EXECUTION_TARGET_UNRESOLVED`, and no program |
| R163 | §18 R163 | `test_process_exit_freeze.cc`, `R163_descriptor_registry_mismatches_are_rejected`, `compile_fail/owned_process_exit_canonical.cc` | host + ASan/UBSan | CANDIDATE PASS / pending re-freeze | `make recovery-test recovery-compile-fail-test recovery-sanitize-test`: double-read binding, fd RAII, descriptor/registry agreement, and exact missing-overload rejection |
| R164 | §18 R164 | `test_process_exit_observation.cc` (Task 5) | host + AArch64 compile/link | CANDIDATE PASS / pending re-freeze | `make recovery-test recovery-sanitize-test recovery-cross-check`: closed fd, poll errno, not-exited, normal exit, signaled exit, observation-vs-declaration matching, and proof primitive target compilation pass; target runtime remains NOT_RUN |
| R165 | §18 R165 | `test_fault_batch_freeze.cc` | host + ASan/UBSan | CANDIDATE PASS / pending re-freeze | `make -B recovery-test recovery-sanitize-test`, plus `make recovery-compile-fail-test recovery-task4-boundary`: single-source snapshot publication, identity issuance, empty/malformed batch rejection, Phase A preservation, post-FROZEN rejection, private identity construction, production seam absence, and replay-token absence |
| §19.1 | §19.1 | TBD | RK3588 target | UNVERIFIED | — |
| §19.2 | §19.2 | TBD | RK3588 target | UNVERIFIED | — |
| §19.3 | §19.3 | TBD | RK3588 target | UNVERIFIED | — |
| §19.4 | §19.4 | TBD | RK3588 target | UNVERIFIED | — |
| §19.5 | §19.5 | TBD | RK3588 target | UNVERIFIED | — |
| §19.6 | §19.6 | TBD | RK3588 target | UNVERIFIED | — |
| §19.7 | §19.7 | TBD | RK3588 target | UNVERIFIED | — |
| §19.8 | §19.8 | TBD | RK3588 target | UNVERIFIED | — |
| §19.9 | §19.9 | TBD | RK3588 target | UNVERIFIED | — |
| §19.10 | §19.10 | TBD | RK3588 target | UNVERIFIED | — |
| §19.11 | §19.11 | TBD | RK3588 target | UNVERIFIED | — |
| §19.12 | §19.12 | TBD | RK3588 target | UNVERIFIED | — |
| §19.13 | §19.13 | TBD | RK3588 target | UNVERIFIED | — |
| §19.14 | §19.14 | TBD | RK3588 target | UNVERIFIED | — |
| §19.15 | §19.15 | TBD | RK3588 target | UNVERIFIED | — |
| §19.16 | §19.16 | TBD | RK3588 target | UNVERIFIED | — |
| §19.17 | §19.17 | TBD | RK3588 target | UNVERIFIED | — |
| §19.18 | §19.18 | TBD | RK3588 target | UNVERIFIED | — |
| §19.19 | §19.19 | TBD | RK3588 target | UNVERIFIED | — |
| §19.20 | §19.20 | TBD | RK3588 target | UNVERIFIED | — |
| §19.21 | §19.21 | TBD | RK3588 target | UNVERIFIED | — |

Note: Task 3's "no fd in the compiler/canonical translation units" boundary is **not** an R rule. Its evidence is the source scan `tests/test_recovery_task3_boundary.sh` (zero matches required, failing output must name file + line). That scan and R163's compile-fail case (`compile_fail/owned_process_exit_canonical.cc`, which must fail with `no matching function for call to 'canonical_write'`) prove different things and may not substitute for each other.
