#include "recovery/compiler.hpp"
#include "test_support.hpp"

#include <memory>

namespace {

recovery::FrozenRuntimeSnapshot snapshot_with_bindings() {
    recovery::FrozenRuntimeSnapshot snapshot{};
    snapshot.recovery_id = {7, 8, 9};
    snapshot.freeze_seq = 10;
    snapshot.boot_id = 7;
    snapshot.fault_batch = {
        {{7, 1}, recovery::FaultKind::PACKET_DOMAIN_FAULT,
         {1, std::nullopt, std::nullopt}, {1, 2, 3, 4}, {5, {}}}};
    snapshot.owner_executor_bindings = {
        {1, recovery::ExecutorBindingStatus::RESOLVED,
         recovery::ExecutorPrincipal{recovery::ProcessPrincipal{{50, 500}}}, 1}};
    snapshot.epoch_snapshot = {1, 2, 3, 4};
    snapshot.process_exit_bindings = {
        {{{7, 8, 9}, 10, 0}, {100, 1000}},
        {{{7, 8, 9}, 10, 1}, {200, 2000}},
    };
    return snapshot;
}

recovery::CompileResult compile(const recovery::FrozenRuntimeSnapshot& snapshot) {
    auto policy_contents = std::make_shared<recovery::PolicyContents>();
    policy_contents->dispositions.emplace(
        recovery::FaultKind::PACKET_DOMAIN_FAULT,
        recovery::FaultDisposition::COVERED);
    policy_contents->recipes.emplace(
        std::make_pair(recovery::ResourceKind::PACKET_DOMAIN,
                       recovery::RecoveryMode::PACKET_RESET),
        recovery::CanonicalActionRecipe{
            recovery::ResourceKind::PACKET_DOMAIN,
            recovery::RecoveryMode::PACKET_RESET,
            {{recovery::ActionPhase::TEARDOWN, recovery::OperationKind::PACKET_STOP, {}},
             {recovery::ActionPhase::INVALIDATE_EPOCH,
              recovery::OperationKind::INVALIDATE_EPOCH, {0}},
             {recovery::ActionPhase::REINIT, recovery::OperationKind::REINIT, {1}}}});
    auto registry_contents = std::make_shared<recovery::CapabilityRegistryContents>();
    recovery::PolicySnapshot policy{1, recovery::canonical_digest(*policy_contents), policy_contents};
    recovery::CapabilityRegistrySnapshot registry{
        1, recovery::canonical_digest(*registry_contents), registry_contents};
    return recovery::RecoveryCompiler::compile(snapshot, policy, registry, 0);
}

void check_unresolved(recovery::FrozenRuntimeSnapshot snapshot) {
    const auto result = compile(snapshot);
    RECOVERY_CHECK(std::holds_alternative<recovery::CompileFailure>(result));
    const auto& failure = std::get<recovery::CompileFailure>(result);
    RECOVERY_CHECK_EQ(failure.kind,
                      recovery::CompileFailure::Kind::PREDICATE_DERIVATION_FAILED);
    RECOVERY_CHECK(failure.predicate_failure.has_value());
    RECOVERY_CHECK_EQ(failure.predicate_failure->kind,
                      recovery::PredicateDerivationFailureKind::EXECUTION_TARGET_UNRESOLVED);
    RECOVERY_CHECK(!std::holds_alternative<recovery::ValidatedRecoveryProgram>(result));
}

RECOVERY_TEST(R162_duplicate_process_identity_is_unresolved) {
    auto snapshot = snapshot_with_bindings();
    snapshot.process_exit_bindings[1].process = snapshot.process_exit_bindings[0].process;
    check_unresolved(std::move(snapshot));
}

RECOVERY_TEST(R162_reused_binding_id_is_unresolved) {
    auto snapshot = snapshot_with_bindings();
    snapshot.process_exit_bindings[1].id = snapshot.process_exit_bindings[0].id;
    check_unresolved(std::move(snapshot));
}

RECOVERY_TEST(R162_binding_header_mismatch_is_unresolved) {
    auto snapshot = snapshot_with_bindings();
    snapshot.process_exit_bindings[0].id.freeze_seq++;
    check_unresolved(std::move(snapshot));
}

RECOVERY_TEST(R162_binding_seq_mismatch_is_unresolved) {
    auto snapshot = snapshot_with_bindings();
    snapshot.process_exit_bindings[1].id.binding_seq = 8;
    check_unresolved(std::move(snapshot));
}

RECOVERY_TEST(R162_process_order_not_descriptor_order_is_enforced) {
    auto snapshot = snapshot_with_bindings();
    const auto first_id = snapshot.process_exit_bindings[0].id;
    const auto second_id = snapshot.process_exit_bindings[1].id;
    std::swap(snapshot.process_exit_bindings[0].process,
              snapshot.process_exit_bindings[1].process);
    RECOVERY_CHECK_EQ(snapshot.process_exit_bindings[0].id, first_id);
    RECOVERY_CHECK_EQ(snapshot.process_exit_bindings[1].id, second_id);
    RECOVERY_CHECK(!(snapshot.process_exit_bindings[0].process
                     < snapshot.process_exit_bindings[1].process));
    check_unresolved(std::move(snapshot));
}

RECOVERY_TEST(R162_missing_process_descriptor_is_unresolved) {
    auto snapshot = snapshot_with_bindings();
    const recovery::ProcessIdentity missing{300, 3000};
    snapshot.ledger_prefix.push_back({1, std::nullopt, {recovery::SubjectIdentity{missing}}, {}});
    check_unresolved(std::move(snapshot));
}

RECOVERY_TEST(R9_snapshot_canonical_order_pins_process_bindings_between_owner_and_epoch) {
    const auto snapshot = snapshot_with_bindings();
    recovery::CanonicalWriter expected;
    recovery::canonical_write(expected, snapshot.recovery_id);
    recovery::canonical_write(expected, snapshot.freeze_seq);
    recovery::canonical_write(expected, snapshot.boot_id);
    recovery::canonical_write(expected, snapshot.fault_batch);
    recovery::canonical_write(expected, snapshot.owner_executor_bindings);
    recovery::canonical_write(expected, snapshot.process_exit_bindings);
    recovery::canonical_write(expected, snapshot.epoch_snapshot);
    recovery::canonical_write(expected, snapshot.snapshot_digest);
    recovery::canonical_write(expected, snapshot.ledger_prefix);
    RECOVERY_CHECK_EQ(recovery::canonical_bytes(snapshot), std::move(expected).finish());
}

}  // namespace
