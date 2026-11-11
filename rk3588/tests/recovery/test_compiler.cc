#include "recovery/compiler.hpp"
#include "test_support.hpp"

#include <limits>
#include <memory>
#include <type_traits>
#include <variant>

namespace {

struct Inputs {
    recovery::FrozenRuntimeSnapshot snapshot;
    recovery::PolicySnapshot policy;
    recovery::CapabilityRegistrySnapshot registry;
};

Inputs valid_inputs() {
    const recovery::FaultFact fault{{7, 1}, recovery::FaultKind::PACKET_DOMAIN_FAULT,
                                    {42, std::nullopt, std::nullopt},
                                    {3, 4, 5, 6}, {9, {}}};
    auto policy_contents = std::make_shared<recovery::PolicyContents>();
    policy_contents->dispositions.emplace(recovery::FaultKind::PACKET_DOMAIN_FAULT,
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
    const recovery::Digest empty_digest{};
    return Inputs{
        recovery::FrozenRuntimeSnapshot{{7, 1, 99}, 2, 7, {fault},
                                        {{42, recovery::ExecutorBindingStatus::RESOLVED,
                                          recovery::ExecutorPrincipal{
                                              recovery::ProcessPrincipal{{123, 456}}}, 1}},
                                        {}, {3, 4, 5, 6}, empty_digest},
        recovery::PolicySnapshot{3, recovery::canonical_digest(*policy_contents),
                                 std::move(policy_contents)},
        recovery::CapabilityRegistrySnapshot{
            4, recovery::canonical_digest(*registry_contents), std::move(registry_contents)}};
}

RECOVERY_TEST(R2_R7_empty_fault_is_explicit_failure) {
    static_assert(!std::is_default_constructible_v<recovery::CompileResult>);
    auto inputs = valid_inputs();
    inputs.snapshot.fault_batch.clear();
    const auto result = recovery::RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, inputs.registry, 100);
    RECOVERY_CHECK(std::holds_alternative<recovery::CompileFailure>(result));
    RECOVERY_CHECK_EQ(std::get<recovery::CompileFailure>(result).kind,
                      recovery::CompileFailure::Kind::EMPTY_FAULT_BATCH);
}

RECOVERY_TEST(R9_same_inputs_compile_byte_identically) {
    const auto inputs = valid_inputs();
    auto first = recovery::RecoveryCompiler::compile(inputs.snapshot, inputs.policy, inputs.registry, 100);
    auto second = recovery::RecoveryCompiler::compile(inputs.snapshot, inputs.policy, inputs.registry, 100);
    RECOVERY_CHECK(std::holds_alternative<recovery::ValidatedRecoveryProgram>(first));
    RECOVERY_CHECK(std::holds_alternative<recovery::ValidatedRecoveryProgram>(second));
    const auto& a = std::get<recovery::ValidatedRecoveryProgram>(first);
    const auto& b = std::get<recovery::ValidatedRecoveryProgram>(second);
    RECOVERY_CHECK_EQ(a.transaction_hash(), b.transaction_hash());
    RECOVERY_CHECK_EQ(a.resource_plans(), b.resource_plans());
    RECOVERY_CHECK_EQ(a.action_dag(), b.action_dag());
    RECOVERY_CHECK_EQ(a.faults(), b.faults());
    RECOVERY_CHECK_EQ(a.fault_coverage(), b.fault_coverage());
    RECOVERY_CHECK_EQ(a.execution_predicates(), b.execution_predicates());
    RECOVERY_CHECK_EQ(a.device_predicates(), b.device_predicates());
    RECOVERY_CHECK_EQ(a.capability_expectations(), b.capability_expectations());
    RECOVERY_CHECK_EQ(recovery::canonical_bytes(a.faults()), recovery::canonical_bytes(b.faults()));
    RECOVERY_CHECK_EQ(recovery::canonical_bytes(a.resource_plans()), recovery::canonical_bytes(b.resource_plans()));
    RECOVERY_CHECK_EQ(recovery::canonical_bytes(a.fault_coverage()), recovery::canonical_bytes(b.fault_coverage()));
    RECOVERY_CHECK_EQ(recovery::canonical_bytes(a.execution_predicates()), recovery::canonical_bytes(b.execution_predicates()));
    RECOVERY_CHECK_EQ(recovery::canonical_bytes(a.device_predicates()), recovery::canonical_bytes(b.device_predicates()));
    RECOVERY_CHECK_EQ(recovery::canonical_bytes(a.capability_expectations()), recovery::canonical_bytes(b.capability_expectations()));
    RECOVERY_CHECK_EQ(recovery::canonical_bytes(a.action_dag()), recovery::canonical_bytes(b.action_dag()));
}

RECOVERY_TEST(R7_every_compile_failure_kind_is_an_explicit_error_alternative) {
    const recovery::CompileFailure::Kind kinds[] = {
        recovery::CompileFailure::Kind::EMPTY_FAULT_BATCH,
        recovery::CompileFailure::Kind::FALLBACK_REQUIRED,
        recovery::CompileFailure::Kind::EMPTY_PLAN,
        recovery::CompileFailure::Kind::ACTION_CLOSURE_FAILED,
        recovery::CompileFailure::Kind::INVALID_DAG,
        recovery::CompileFailure::Kind::PREDICATE_DERIVATION_FAILED,
        recovery::CompileFailure::Kind::TARGET_PREDICATE_MISMATCH,
        recovery::CompileFailure::Kind::ALLOCATION_FAILED,
    };
    for (const auto kind : kinds) {
        recovery::CompileResult result{recovery::CompileFailure{kind}};
        RECOVERY_CHECK(std::holds_alternative<recovery::CompileFailure>(result));
        RECOVERY_CHECK_EQ(std::get<recovery::CompileFailure>(result).kind, kind);
    }
}

RECOVERY_TEST(transaction_hash_covers_registry_version) {
    auto first_inputs = valid_inputs();
    auto second_inputs = valid_inputs();
    second_inputs.registry.version += 1;
    auto first = recovery::RecoveryCompiler::compile(
        first_inputs.snapshot, first_inputs.policy, first_inputs.registry, 100);
    auto second = recovery::RecoveryCompiler::compile(
        second_inputs.snapshot, second_inputs.policy, second_inputs.registry, 100);
    RECOVERY_CHECK(std::holds_alternative<recovery::ValidatedRecoveryProgram>(first));
    RECOVERY_CHECK(std::holds_alternative<recovery::ValidatedRecoveryProgram>(second));
    RECOVERY_CHECK(std::get<recovery::ValidatedRecoveryProgram>(first).transaction_hash()
                   != std::get<recovery::ValidatedRecoveryProgram>(second).transaction_hash());
}

RECOVERY_TEST(non_worker_process_subject_is_not_an_execution_proof_target) {
    auto inputs = valid_inputs();
    inputs.snapshot.fault_batch.front().subject.process = recovery::ProcessIdentity{123, 456};
    const auto result = recovery::RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, inputs.registry, 100);
    RECOVERY_CHECK(std::holds_alternative<recovery::ValidatedRecoveryProgram>(result));
    RECOVERY_CHECK(std::get<recovery::ValidatedRecoveryProgram>(result)
                       .execution_predicates().empty());
}

RECOVERY_TEST(fallback_uses_worst_severity_and_aggregates_triggering_faults) {
    auto inputs = valid_inputs();
    auto first = inputs.snapshot.fault_batch.front();
    auto second = first;
    first.id.fault_seq = 10;
    first.kind = recovery::FaultKind::UNKNOWN;
    second.id.fault_seq = 11;
    second.kind = recovery::FaultKind::CAPABILITY_FAULT;
    inputs.snapshot.fault_batch = {first, second};
    auto contents = std::make_shared<recovery::PolicyContents>(*inputs.policy.contents);
    contents->fallbacks[recovery::FaultKind::UNKNOWN] =
        recovery::FallbackKind::PARENT_SERVICE_RESTART;
    contents->fallbacks[recovery::FaultKind::CAPABILITY_FAULT] =
        recovery::FallbackKind::BOARD_RESTART_REQUIRED;
    inputs.policy.contents = contents;
    inputs.policy.digest = recovery::canonical_digest(*contents);
    const auto result = recovery::RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, inputs.registry, 100);
    const auto& failure = std::get<recovery::CompileFailure>(result);
    RECOVERY_CHECK_EQ(failure.kind, recovery::CompileFailure::Kind::FALLBACK_REQUIRED);
    RECOVERY_CHECK(failure.fallback.has_value());
    RECOVERY_CHECK_EQ(failure.fallback->kind,
                      recovery::FallbackKind::BOARD_RESTART_REQUIRED);
    const std::set<recovery::FaultIdentity> expected_faults{first.id, second.id};
    RECOVERY_CHECK_EQ(failure.fallback->triggering_faults, expected_faults);
}

RECOVERY_TEST(disconnected_local_recipe_is_rejected_as_invalid_dag) {
    auto inputs = valid_inputs();
    auto contents = std::make_shared<recovery::PolicyContents>(*inputs.policy.contents);
    contents->recipes.begin()->second.actions.back().prerequisite_indices.clear();
    inputs.policy.contents = contents;
    inputs.policy.digest = recovery::canonical_digest(*contents);
    const auto result = recovery::RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, inputs.registry, 100);
    RECOVERY_CHECK(std::holds_alternative<recovery::CompileFailure>(result));
    RECOVERY_CHECK_EQ(std::get<recovery::CompileFailure>(result).kind,
                      recovery::CompileFailure::Kind::INVALID_DAG);
}

RECOVERY_TEST(device_predicate_is_derived_from_ledger_and_registry_evidence) {
    auto inputs = valid_inputs();
    constexpr recovery::DeviceContextIdentity device = 77;
    constexpr recovery::CapabilityId capability = 5;
    inputs.snapshot.fault_batch.front().subject.device_context = device;
    inputs.snapshot.ledger_prefix.push_back(
        recovery::FrozenGrantRecord{42, std::nullopt, {}, {{device}}});

    auto policy = std::make_shared<recovery::PolicyContents>(*inputs.policy.contents);
    policy->device_capability_map.emplace(device, capability);
    policy->resource_device_proof_kind.emplace(
        recovery::ResourceKind::PACKET_DOMAIN,
        recovery::DeviceProofKind::CONTEXT_DESTROY_CONFIRMED);
    inputs.policy.contents = policy;
    inputs.policy.digest = recovery::canonical_digest(*policy);

    recovery::CapabilityEvidence evidence{
        capability, device, {{1}, {2}, {3}, {4}, 9}, 12, {8}, 50, 150};
    auto registry = std::make_shared<recovery::CapabilityRegistryContents>();
    registry->evidences.push_back(evidence);
    inputs.registry.contents = registry;
    inputs.registry.digest = recovery::canonical_digest(*registry);

    auto result = recovery::RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, inputs.registry, 100);
    RECOVERY_CHECK(std::holds_alternative<recovery::ValidatedRecoveryProgram>(result));
    const auto& program = std::get<recovery::ValidatedRecoveryProgram>(result);
    RECOVERY_CHECK_EQ(program.device_predicates().size(), 1U);
    const recovery::PredicateKey expected_key = recovery::DevicePredicateKey{
        recovery::DeviceProofKind::CONTEXT_DESTROY_CONFIRMED, device};
    RECOVERY_CHECK_EQ(program.device_predicates().front().key, expected_key);
    RECOVERY_CHECK_EQ(program.device_predicates().front().expected_capability.evidence_id,
                      evidence.evidence_id);
    RECOVERY_CHECK_EQ(program.capability_expectations().size(), 1U);
    RECOVERY_CHECK_EQ(program.capability_expectations().front().evidence_digest,
                      evidence.evidence_digest);
}

RECOVERY_TEST(device_fault_target_outside_ledger_closure_fails_explicitly) {
    auto inputs = valid_inputs();
    inputs.snapshot.fault_batch.front().subject.device_context = 77;
    const auto result = recovery::RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, inputs.registry, 100);
    const auto& failure = std::get<recovery::CompileFailure>(result);
    RECOVERY_CHECK_EQ(failure.kind,
                      recovery::CompileFailure::Kind::PREDICATE_DERIVATION_FAILED);
    RECOVERY_CHECK(failure.predicate_failure.has_value());
    RECOVERY_CHECK_EQ(failure.predicate_failure->kind,
                      recovery::PredicateDerivationFailureKind::TARGET_NOT_IN_CLOSURE);
}

RECOVERY_TEST(process_execution_predicate_is_derived_from_ledger_closure) {
    auto inputs = valid_inputs();
    const recovery::ProcessIdentity process{123, 456};
    inputs.snapshot.process_exit_bindings.push_back(
        {{inputs.snapshot.recovery_id, inputs.snapshot.freeze_seq, 0}, process});
    inputs.snapshot.ledger_prefix.push_back(
        recovery::FrozenGrantRecord{42, std::nullopt,
                                    {recovery::SubjectIdentity{process}}, {}});
    inputs.snapshot.process_exit_bindings = {
        {{inputs.snapshot.recovery_id, inputs.snapshot.freeze_seq, 0}, process}};
    const auto result = recovery::RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, inputs.registry, 100);
    RECOVERY_CHECK(std::holds_alternative<recovery::ValidatedRecoveryProgram>(result));
    const auto& predicates =
        std::get<recovery::ValidatedRecoveryProgram>(result).execution_predicates();
    RECOVERY_CHECK_EQ(predicates.size(), 1U);
    const recovery::PredicateKey expected_key = recovery::ExecutionPredicateKey{
        recovery::ExecutionProofKind::PROCESS_EXIT_CONFIRMED,
        recovery::SubjectIdentity{process}};
    RECOVERY_CHECK_EQ(predicates.front().key, expected_key);
    RECOVERY_CHECK_EQ(predicates.front().expected_spawn_generation,
                      std::optional<std::uint64_t>{process.start_generation});
    const std::optional<recovery::ProcessExitBindingId> expected_binding{
        recovery::ProcessExitBindingId{inputs.snapshot.recovery_id,
                                       inputs.snapshot.freeze_seq, 0}};
    RECOVERY_CHECK_EQ(predicates.front().expected_process_binding, expected_binding);
}

RECOVERY_TEST(R127_missing_recipe_is_explicit_closure_failure) {
    auto inputs = valid_inputs();
    auto contents = std::make_shared<recovery::PolicyContents>();
    contents->dispositions.emplace(recovery::FaultKind::PACKET_DOMAIN_FAULT,
                                   recovery::FaultDisposition::COVERED);
    inputs.policy.contents = std::move(contents);
    inputs.policy.digest = recovery::canonical_digest(*inputs.policy.contents);
    auto result = recovery::RecoveryCompiler::compile(inputs.snapshot, inputs.policy, inputs.registry, 100);
    RECOVERY_CHECK(std::holds_alternative<recovery::CompileFailure>(result));
    const auto& failure = std::get<recovery::CompileFailure>(result);
    RECOVERY_CHECK_EQ(failure.kind, recovery::CompileFailure::Kind::ACTION_CLOSURE_FAILED);
    RECOVERY_CHECK(failure.closure_failure.has_value());
    RECOVERY_CHECK_EQ(failure.closure_failure->kind,
                      recovery::ActionClosureFailureKind::RECIPE_MISSING);
}

RECOVERY_TEST(R127_missing_binding_is_explicit_closure_failure) {
    auto inputs = valid_inputs();
    inputs.snapshot.owner_executor_bindings.clear();
    const auto result = recovery::RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, inputs.registry, 100);
    const auto& failure = std::get<recovery::CompileFailure>(result);
    RECOVERY_CHECK_EQ(failure.kind, recovery::CompileFailure::Kind::ACTION_CLOSURE_FAILED);
    RECOVERY_CHECK_EQ(failure.closure_failure->kind,
                      recovery::ActionClosureFailureKind::EXECUTOR_BINDING_MISSING);
}

RECOVERY_TEST(R127_ambiguous_binding_is_explicit_closure_failure) {
    auto inputs = valid_inputs();
    inputs.snapshot.owner_executor_bindings.front().status =
        recovery::ExecutorBindingStatus::AMBIGUOUS;
    const auto result = recovery::RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, inputs.registry, 100);
    const auto& failure = std::get<recovery::CompileFailure>(result);
    RECOVERY_CHECK_EQ(failure.closure_failure->kind,
                      recovery::ActionClosureFailureKind::EXECUTOR_BINDING_AMBIGUOUS);
}

RECOVERY_TEST(R127_unauthenticatable_binding_is_explicit_closure_failure) {
    auto inputs = valid_inputs();
    inputs.snapshot.owner_executor_bindings.front().status =
        recovery::ExecutorBindingStatus::UNAUTHENTICATABLE;
    const auto result = recovery::RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, inputs.registry, 100);
    const auto& failure = std::get<recovery::CompileFailure>(result);
    RECOVERY_CHECK_EQ(failure.closure_failure->kind,
                      recovery::ActionClosureFailureKind::EXECUTOR_BINDING_UNAUTHENTICATABLE);
}

RECOVERY_TEST(R127_epoch_overflow_is_explicit_closure_failure) {
    auto inputs = valid_inputs();
    inputs.snapshot.fault_batch.front().observed_epochs.stream_epoch =
        std::numeric_limits<std::uint64_t>::max();
    inputs.snapshot.epoch_snapshot.stream_epoch = std::numeric_limits<std::uint64_t>::max();
    const auto result = recovery::RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, inputs.registry, 100);
    const auto& failure = std::get<recovery::CompileFailure>(result);
    RECOVERY_CHECK_EQ(failure.closure_failure->kind,
                      recovery::ActionClosureFailureKind::EPOCH_OVERFLOW);
}

RECOVERY_TEST(R127_bad_recipe_index_is_explicit_closure_failure) {
    auto inputs = valid_inputs();
    auto contents = std::make_shared<recovery::PolicyContents>(*inputs.policy.contents);
    contents->recipes.begin()->second.actions.front().prerequisite_indices = {7};
    inputs.policy.contents = contents;
    inputs.policy.digest = recovery::canonical_digest(*contents);
    const auto result = recovery::RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, inputs.registry, 100);
    const auto& failure = std::get<recovery::CompileFailure>(result);
    RECOVERY_CHECK_EQ(failure.closure_failure->kind,
                      recovery::ActionClosureFailureKind::RECIPE_MISSING);
}

}  // namespace

namespace recovery {

class RecoveryCompilerTestAccess {
    class ThrowAtStage final : public RecoveryCompiler::CompilerAllocationBoundary {
    public:
        explicit ThrowAtStage(CompilerAllocationStage stage) : stage_(stage) {}
        void checkpoint(CompilerAllocationStage stage) override {
            if (stage == stage_) throw std::bad_alloc{};
        }
    private:
        CompilerAllocationStage stage_;
    };
    class PredicateMutation final : public RecoveryCompiler::CompilerAllocationBoundary {
    public:
        explicit PredicateMutation(bool replace) : replace_(replace) {}
        void checkpoint(CompilerAllocationStage) override {}
        void mutate_predicates_for_test(DerivedPredicates& predicates) override {
            if (replace_) {
                predicates.execution.front().expected_subject = ProcessIdentity{999, 999};
            } else {
                predicates.execution.clear();
            }
        }
    private:
        bool replace_;
    };
public:
    static CompileResult fail_at(const FrozenRuntimeSnapshot& snapshot,
                                 const PolicySnapshot& policy,
                                 const CapabilityRegistrySnapshot& registry,
                                 TrustedMonotonicTime now,
                                 CompilerAllocationStage stage) {
        ThrowAtStage boundary(stage);
        return RecoveryCompiler::compile_impl(snapshot, policy, registry, now, boundary);
    }
    static CompileResult mutate_predicates(const FrozenRuntimeSnapshot& snapshot,
                                           const PolicySnapshot& policy,
                                           const CapabilityRegistrySnapshot& registry,
                                           TrustedMonotonicTime now,
                                           bool replace) {
        PredicateMutation boundary(replace);
        return RecoveryCompiler::compile_impl(snapshot, policy, registry, now, boundary);
    }
};

}  // namespace recovery

namespace {

RECOVERY_TEST(R138_allocation_failure_maps_to_compile_failure) {
    const auto inputs = valid_inputs();
    const auto result = recovery::RecoveryCompilerTestAccess::fail_at(
        inputs.snapshot, inputs.policy, inputs.registry, 100,
        recovery::CompilerAllocationStage::DERIVE_PLANS);
    RECOVERY_CHECK(std::holds_alternative<recovery::CompileFailure>(result));
    RECOVERY_CHECK_EQ(std::get<recovery::CompileFailure>(result).kind,
                      recovery::CompileFailure::Kind::ALLOCATION_FAILED);
}

RECOVERY_TEST(R138_all_compile_allocation_stages_fail_explicitly) {
    const auto inputs = valid_inputs();
    const recovery::CompilerAllocationStage stages[] = {
        recovery::CompilerAllocationStage::DERIVE_PLANS,
        recovery::CompilerAllocationStage::ACTION_CLOSURE,
        recovery::CompilerAllocationStage::DERIVE_PREDICATES,
        recovery::CompilerAllocationStage::CONSTRUCT_PROGRAM,
    };
    for (const auto stage : stages) {
        const auto result = recovery::RecoveryCompilerTestAccess::fail_at(
            inputs.snapshot, inputs.policy, inputs.registry, 100, stage);
        RECOVERY_CHECK(std::holds_alternative<recovery::CompileFailure>(result));
        RECOVERY_CHECK_EQ(std::get<recovery::CompileFailure>(result).kind,
                          recovery::CompileFailure::Kind::ALLOCATION_FAILED);
    }
}

RECOVERY_TEST(R56_missing_or_replaced_predicate_is_rejected) {
    for (const bool replace : {false, true}) {
        auto inputs = valid_inputs();
        const recovery::ProcessIdentity process{123, 456};
        inputs.snapshot.process_exit_bindings = {
            {{inputs.snapshot.recovery_id, inputs.snapshot.freeze_seq, 0}, process}};
        inputs.snapshot.ledger_prefix.push_back(
            {42, std::nullopt, {recovery::SubjectIdentity{process}}, {}});
        const auto result = recovery::RecoveryCompilerTestAccess::mutate_predicates(
            inputs.snapshot, inputs.policy, inputs.registry, 100, replace);
        RECOVERY_CHECK(std::holds_alternative<recovery::CompileFailure>(result));
        RECOVERY_CHECK_EQ(std::get<recovery::CompileFailure>(result).kind,
                          recovery::CompileFailure::Kind::TARGET_PREDICATE_MISMATCH);
    }
}

}  // namespace
