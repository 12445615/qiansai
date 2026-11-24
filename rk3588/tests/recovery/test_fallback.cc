#include "recovery/runtime.hpp"
#include "test_support.hpp"

namespace {
recovery::FaultFact fault(std::uint64_t seq, recovery::FaultKind kind,
                          recovery::ResourceInstance resource) {
    return recovery::RecoveryTestAccess::make_fault(
        7, seq, {kind, {resource, std::nullopt, std::nullopt},
                 {1, 2, 3, 4}, {seq, {}}});
}
recovery::FaultReport report(recovery::FaultKind kind, recovery::ResourceInstance resource) {
    return {kind, {resource, std::nullopt, std::nullopt}, {1, 2, 3, 4}, {1, {}}};
}
recovery::FrozenRuntimeSnapshot snapshot(std::vector<recovery::FaultFact> faults) {
    return {{7, 1, 9}, 1, 7, 0, 0, {}, {}, {}, std::move(faults), 0, {},
            {}, {}, {}, {}};
}
recovery::PolicySnapshot policy(
    std::map<recovery::FaultKind, recovery::FaultDisposition> dispositions,
    std::map<recovery::FaultKind, recovery::FallbackKind> fallbacks = {}) {
    auto contents = std::make_shared<recovery::PolicyContents>();
    contents->dispositions = std::move(dispositions);
    contents->fallbacks = std::move(fallbacks);
    return {1, recovery::canonical_digest(*contents), std::move(contents)};
}
}

RECOVERY_TEST(R15_R16_online_derivation_has_exact_coverage_and_no_fallback) {
    auto snap = snapshot({fault(1, recovery::FaultKind::PACKET_DOMAIN_FAULT, 10),
                          fault(2, recovery::FaultKind::RGA_DEVICE_FAULT, 20)});
    auto pol = policy({{recovery::FaultKind::PACKET_DOMAIN_FAULT, recovery::FaultDisposition::COVERED},
                       {recovery::FaultKind::RGA_DEVICE_FAULT, recovery::FaultDisposition::ESCALATED}});
    const auto result = recovery::derive_resource_plans(snap, snap.fault_batch, pol);
    RECOVERY_CHECK_EQ(result.outcome, recovery::DerivationOutcome::ONLINE_RECOVERY);
    RECOVERY_CHECK_EQ(result.fault_coverage.entries.size(), 2U);
    RECOVERY_CHECK_EQ(result.resource_plans.size(), 2U);
    RECOVERY_CHECK(!result.fallback.has_value());
}

RECOVERY_TEST(R17_R18_R39_mixed_unrecoverable_chooses_board_and_no_online_plan) {
    auto snap = snapshot({fault(1, recovery::FaultKind::PACKET_DOMAIN_FAULT, 10),
                          fault(2, recovery::FaultKind::WORKER_PROCESS_EXIT, 20),
                          fault(3, recovery::FaultKind::CAPABILITY_FAULT, 30)});
    auto pol = policy({{recovery::FaultKind::PACKET_DOMAIN_FAULT, recovery::FaultDisposition::COVERED},
                       {recovery::FaultKind::WORKER_PROCESS_EXIT, recovery::FaultDisposition::UNRECOVERABLE},
                       {recovery::FaultKind::CAPABILITY_FAULT, recovery::FaultDisposition::UNRECOVERABLE}},
                      {{recovery::FaultKind::WORKER_PROCESS_EXIT, recovery::FallbackKind::PARENT_SERVICE_RESTART},
                       {recovery::FaultKind::CAPABILITY_FAULT, recovery::FallbackKind::BOARD_RESTART_REQUIRED}});
    const auto result = recovery::derive_resource_plans(snap, snap.fault_batch, pol);
    RECOVERY_CHECK_EQ(result.outcome, recovery::DerivationOutcome::BOARD_RESTART_REQUIRED);
    RECOVERY_CHECK(result.resource_plans.empty());
    RECOVERY_CHECK_EQ(result.fault_coverage.entries.size(), 3U);
    RECOVERY_CHECK_EQ(result.fallback->kind, recovery::FallbackKind::BOARD_RESTART_REQUIRED);
    RECOVERY_CHECK_EQ(result.fallback->triggering_faults.size(), 2U);
}

RECOVERY_TEST(R17_parent_restart_outcome_is_distinct_and_has_empty_plan) {
    auto snap = snapshot({fault(1, recovery::FaultKind::WORKER_PROCESS_EXIT, 20)});
    auto pol = policy({{recovery::FaultKind::WORKER_PROCESS_EXIT,
                        recovery::FaultDisposition::UNRECOVERABLE}},
                      {{recovery::FaultKind::WORKER_PROCESS_EXIT,
                        recovery::FallbackKind::PARENT_SERVICE_RESTART}});
    const auto result = recovery::derive_resource_plans(snap, snap.fault_batch, pol);
    RECOVERY_CHECK_EQ(result.outcome, recovery::DerivationOutcome::PARENT_SERVICE_RESTART);
    RECOVERY_CHECK(result.resource_plans.empty());
    RECOVERY_CHECK_EQ(result.fallback->kind, recovery::FallbackKind::PARENT_SERVICE_RESTART);
}

RECOVERY_TEST(R16_capability_fault_requires_board_restart_and_has_no_dag_input) {
    auto snap = snapshot({fault(1, recovery::FaultKind::CAPABILITY_FAULT, 20)});
    auto pol = policy({{recovery::FaultKind::CAPABILITY_FAULT, recovery::FaultDisposition::UNRECOVERABLE}},
                      {{recovery::FaultKind::CAPABILITY_FAULT, recovery::FallbackKind::BOARD_RESTART_REQUIRED}});
    const auto result = recovery::derive_resource_plans(snap, snap.fault_batch, pol);
    RECOVERY_CHECK_EQ(result.outcome, recovery::DerivationOutcome::BOARD_RESTART_REQUIRED);
    RECOVERY_CHECK(result.resource_plans.empty());
}

RECOVERY_TEST(R17_known_plus_unknown_does_not_recover_the_known_subset) {
    auto snap = snapshot({fault(1, recovery::FaultKind::PACKET_DOMAIN_FAULT, 10),
                          fault(2, recovery::FaultKind::UNKNOWN, 20)});
    auto pol = policy({{recovery::FaultKind::PACKET_DOMAIN_FAULT, recovery::FaultDisposition::COVERED},
                       {recovery::FaultKind::UNKNOWN, recovery::FaultDisposition::UNRECOVERABLE}});
    const auto result = recovery::derive_resource_plans(snap, snap.fault_batch, pol);
    RECOVERY_CHECK_EQ(result.outcome, recovery::DerivationOutcome::FAIL_CLOSED);
    RECOVERY_CHECK(result.resource_plans.empty());
}

RECOVERY_TEST(R19_R41_unknown_without_action_is_explicit_fail_closed) {
    auto snap = snapshot({fault(1, recovery::FaultKind::UNKNOWN, 10)});
    auto pol = policy({{recovery::FaultKind::UNKNOWN, recovery::FaultDisposition::UNRECOVERABLE}});
    const auto result = recovery::derive_resource_plans(snap, snap.fault_batch, pol);
    RECOVERY_CHECK_EQ(result.outcome, recovery::DerivationOutcome::FAIL_CLOSED);
    RECOVERY_CHECK(result.resource_plans.empty());
    RECOVERY_CHECK(!result.fallback.has_value());
    RECOVERY_CHECK_EQ(result.fault_coverage.entries.size(), 1U);
}

RECOVERY_TEST(R40_finish_compile_records_fallback_until_unique_execute_transition) {
    recovery::RecoveryCoordinator coordinator{7};
    RECOVERY_CHECK(coordinator.report_fault(
        report(recovery::FaultKind::CAPABILITY_FAULT, 10)));
    RECOVERY_CHECK(coordinator.begin_freeze({}));
    RECOVERY_CHECK(coordinator.commit_freeze());
    RECOVERY_CHECK(coordinator.begin_compile());
    const recovery::FallbackAction action{recovery::FallbackKind::BOARD_RESTART_REQUIRED,
                                           {recovery::RecoveryTestAccess::make_fault(
                                               7, 1,
                                               report(recovery::FaultKind::CAPABILITY_FAULT, 10)).id}};
    coordinator.finish_compile(recovery::CompileFailure{
        recovery::CompileFailure::Kind::FALLBACK_REQUIRED, action});
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::COMPILING);
    RECOVERY_CHECK(coordinator.active().compiled_program == nullptr);
    coordinator.execute_fallback(action);
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FAIL_CLOSED);
}

RECOVERY_TEST(R19_fallback_result_cannot_be_sealed_as_a_program) {
    recovery::RecoveryCoordinator coordinator{7};
    RECOVERY_CHECK(coordinator.report_fault(report(recovery::FaultKind::UNKNOWN, 10)));
    RECOVERY_CHECK(coordinator.begin_freeze({}));
    RECOVERY_CHECK(coordinator.commit_freeze());
    RECOVERY_CHECK(coordinator.begin_compile());
    coordinator.finish_compile(recovery::CompileFailure{
        recovery::CompileFailure::Kind::FALLBACK_REQUIRED, std::nullopt});
    RECOVERY_CHECK(coordinator.active().compiled_program == nullptr);
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK(!coordinator.seal_validated_program(nullptr));
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FAIL_CLOSED);
}

RECOVERY_TEST(R41_failed_derivation_cannot_be_disguised_as_compile_success) {
    recovery::RecoveryCoordinator coordinator{7};
    RECOVERY_CHECK(coordinator.report_fault(report(recovery::FaultKind::UNKNOWN, 10)));
    RECOVERY_CHECK(coordinator.begin_freeze({}));
    RECOVERY_CHECK(coordinator.commit_freeze());
    auto pol = policy({{recovery::FaultKind::UNKNOWN,
                        recovery::FaultDisposition::UNRECOVERABLE}});
    auto registry_contents = std::make_shared<recovery::CapabilityRegistryContents>();
    recovery::CapabilityRegistrySnapshot registry{
        1, recovery::canonical_digest(*registry_contents), registry_contents};
    RECOVERY_CHECK(coordinator.begin_compile(pol, registry));
    auto result = recovery::RecoveryCompiler::compile(
        *coordinator.active().current_snapshot, pol, registry, 0);
    RECOVERY_CHECK(std::holds_alternative<recovery::CompileFailure>(result));
    coordinator.finish_compile(std::move(result));
    RECOVERY_CHECK(coordinator.active().compiled_program == nullptr);
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FAIL_CLOSED);
}
