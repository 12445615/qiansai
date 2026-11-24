#include "recovery/runtime.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <memory>

namespace recovery {
class RecoveryRuntimeTestAccess {
public:
    static void change_snapshot_digest(RecoveryCoordinator& value) {
        value.slot_.active->current_snapshot->snapshot_digest[0] ^= 1U;
    }
    static void change_policy_digest(RecoveryCoordinator& value) {
        value.slot_.active->current_policy.digest[0] ^= 1U;
    }
};
class ValidatedRecoveryProgramTestAccess {
public:
    enum class Field { HASH, FAULT, PLAN, PREDICATE, DAG, STREAMOFF, REQBUFS_ZERO };
    static void tamper(ValidatedRecoveryProgram& value, Field field) {
        switch (field) {
        case Field::HASH: value.transaction_hash_[0] ^= 1U; break;
        case Field::FAULT: value.faults_.front().id.fault_seq += 1; break;
        case Field::PLAN: value.resource_plans_.front().resource += 1; break;
        case Field::PREDICATE:
            value.execution_predicates_.push_back(
                {ExecutionPredicateKey{ExecutionProofKind::THREADS_JOINED,
                                       ThreadIdentity{1, 2, 3}},
                 ExecutionProofKind::THREADS_JOINED,
                 ProofIssuer{0, IssuerRole::EXECUTION_AGENT}, ThreadIdentity{1, 2, 3},
                 0, std::nullopt, std::nullopt});
            break;
        case Field::DAG: value.action_dag_.front().id += 1; break;
        case Field::STREAMOFF:
        case Field::REQBUFS_ZERO: {
            const auto operation = field == Field::STREAMOFF
                ? OperationKind::STREAMOFF : OperationKind::REQBUFS_ZERO;
            const auto found = std::find_if(value.action_dag_.begin(), value.action_dag_.end(),
                [operation](const ActionNode& node) { return node.operation == operation; });
            if (found != value.action_dag_.end()) value.action_dag_.erase(found);
            break;
        }
    }
    }
};
}

namespace {
struct ThrowAt final : recovery::RuntimeFailureProvider {
    recovery::RuntimeFailureStage target;
    bool enabled{true};
    explicit ThrowAt(recovery::RuntimeFailureStage value) : target(value) {}
    void checkpoint(recovery::RuntimeFailureStage value) override {
        if (enabled && value == target) throw std::bad_alloc{};
    }
};

struct LifecycleInputs {
    std::unique_ptr<recovery::RecoveryCoordinator> coordinator;
    recovery::PolicySnapshot policy;
    recovery::CapabilityRegistrySnapshot registry;
    explicit LifecycleInputs(recovery::RuntimeFailureProvider* provider = nullptr)
        : coordinator(provider
              ? std::make_unique<recovery::RecoveryCoordinator>(11, nullptr, provider)
              : std::make_unique<recovery::RecoveryCoordinator>(11)) {}
};

LifecycleInputs v4l2_compiling_coordinator() {
    LifecycleInputs in;
    recovery::FaultReport f{recovery::FaultKind::V4L2_DEVICE_FAULT,
                          {1, std::nullopt, std::nullopt}, {1,2,3,4}, {1,{}}};
    auto policy = std::make_shared<recovery::PolicyContents>();
    policy->dispositions[recovery::FaultKind::V4L2_DEVICE_FAULT] = recovery::FaultDisposition::COVERED;
    policy->recipes[{recovery::ResourceKind::V4L2_CAPTURE, recovery::RecoveryMode::CAPTURE_RESET}] =
        {recovery::ResourceKind::V4L2_CAPTURE, recovery::RecoveryMode::CAPTURE_RESET,
         {{recovery::ActionPhase::TEARDOWN, recovery::OperationKind::STREAMOFF, {}},
          {recovery::ActionPhase::TEARDOWN, recovery::OperationKind::REQBUFS_ZERO, {0}},
          {recovery::ActionPhase::INVALIDATE_EPOCH, recovery::OperationKind::INVALIDATE_EPOCH, {1}},
          {recovery::ActionPhase::REINIT, recovery::OperationKind::REINIT, {2}}}};
    auto registry = std::make_shared<recovery::CapabilityRegistryContents>();
    in.policy = {2, recovery::canonical_digest(*policy), policy};
    in.registry = {3, recovery::canonical_digest(*registry), registry};
    RECOVERY_CHECK(in.coordinator->report_fault(f));
    RECOVERY_CHECK(in.coordinator->begin_freeze({},
        {{1, recovery::ExecutorBindingStatus::RESOLVED,
          recovery::ExecutorPrincipal{recovery::ProcessPrincipal{{99, 100}}}, 1}}));
    RECOVERY_CHECK(in.coordinator->commit_freeze());
    RECOVERY_CHECK(in.coordinator->begin_compile(in.policy, in.registry));
    return in;
}

LifecycleInputs compiling_coordinator(recovery::RuntimeFailureProvider* provider = nullptr) {
    LifecycleInputs in{provider};
    recovery::FaultReport f{recovery::FaultKind::PACKET_DOMAIN_FAULT,
                          {1, std::nullopt, std::nullopt}, {1,2,3,4}, {1,{}}};
    auto policy = std::make_shared<recovery::PolicyContents>();
    policy->dispositions[recovery::FaultKind::PACKET_DOMAIN_FAULT] =
        recovery::FaultDisposition::COVERED;
    policy->recipes[std::make_pair(recovery::ResourceKind::PACKET_DOMAIN,
                                    recovery::RecoveryMode::PACKET_RESET)] =
        {recovery::ResourceKind::PACKET_DOMAIN, recovery::RecoveryMode::PACKET_RESET,
         {{recovery::ActionPhase::TEARDOWN, recovery::OperationKind::PACKET_STOP, {}},
          {recovery::ActionPhase::INVALIDATE_EPOCH,
           recovery::OperationKind::INVALIDATE_EPOCH, {0}},
          {recovery::ActionPhase::REINIT, recovery::OperationKind::REINIT, {1}}}};
    auto registry = std::make_shared<recovery::CapabilityRegistryContents>();
    in.policy = {2, recovery::canonical_digest(*policy), policy};
    in.registry = {3, recovery::canonical_digest(*registry), registry};
    RECOVERY_CHECK(in.coordinator->report_fault(f));
    RECOVERY_CHECK(in.coordinator->begin_freeze({},
        {{1, recovery::ExecutorBindingStatus::RESOLVED,
          recovery::ExecutorPrincipal{recovery::ProcessPrincipal{{99, 100}}}, 1}}));
    RECOVERY_CHECK(in.coordinator->commit_freeze());
    RECOVERY_CHECK(in.coordinator->begin_compile(in.policy, in.registry));
    return in;
}

RECOVERY_TEST(runtime_failure_injection_before_initial_publish_fails_closed) {
    ThrowAt failure{recovery::RuntimeFailureStage::REPORT_PREPARE};
    recovery::RecoveryCoordinator coordinator{11, nullptr, &failure};
    recovery::FaultReport f{recovery::FaultKind::PACKET_DOMAIN_FAULT,
                          {1, std::nullopt, std::nullopt}, {1,2,3,4}, {1,{}}};
    RECOVERY_CHECK(!coordinator.report_fault(f));
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK(!coordinator.has_active());
}

RECOVERY_TEST(R137_runtime_failure_matrix_preserves_prepublication_state) {
    recovery::FaultReport f{recovery::FaultKind::PACKET_DOMAIN_FAULT,
                          {1, std::nullopt, std::nullopt}, {1,2,3,4}, {1,{}}};
    {
        ThrowAt failure{recovery::RuntimeFailureStage::REPORT_APPEND};
        recovery::RecoveryCoordinator c{11, nullptr, &failure};
        RECOVERY_CHECK(c.report_fault(f));
        auto second = f; second.evidence.evidence_id = 2;
        RECOVERY_CHECK(!c.report_fault(second));
        RECOVERY_CHECK_EQ(c.active().fault_batch.size(), 1U);
    }
    for (const auto stage : {recovery::RuntimeFailureStage::BEGIN_FREEZE_PREPARE,
                             recovery::RuntimeFailureStage::BEGIN_FREEZE_AFTER_COPY,
                             recovery::RuntimeFailureStage::BEGIN_FREEZE_AFTER_DIGEST}) {
        ThrowAt failure{stage};
        recovery::RecoveryCoordinator c{11, nullptr, &failure};
        RECOVERY_CHECK(c.report_fault(f));
        const std::vector<recovery::FrozenGrantRecord> ledger{
            {1, std::nullopt, {}, {}, {11, 0, 1}, 7, std::nullopt}};
        const std::vector<recovery::OwnerExecutorBinding> bindings{
            {1, recovery::ExecutorBindingStatus::RESOLVED,
             recovery::ExecutorPrincipal{recovery::ProcessPrincipal{{99, 1}}}, 1}};
        RECOVERY_CHECK(!c.begin_freeze(ledger, bindings));
        RECOVERY_CHECK_EQ(c.state(), recovery::RecoverySlotState::PREPARING);
        RECOVERY_CHECK(c.active().ledger_prefix.empty());
        RECOVERY_CHECK(c.active().owner_executor_bindings.empty());
        RECOVERY_CHECK(!c.active().freeze_request.has_value());
    }
    {
        ThrowAt failure{recovery::RuntimeFailureStage::BEGIN_COMPILE_PUBLISH};
        failure.enabled = false;
        recovery::RecoveryCoordinator c{11, nullptr, &failure};
        RECOVERY_CHECK(c.report_fault(f)); RECOVERY_CHECK(c.begin_freeze({}));
        RECOVERY_CHECK(c.commit_freeze());
        failure.enabled = true;
        RECOVERY_CHECK(!c.begin_compile());
        RECOVERY_CHECK_EQ(c.state(), recovery::RecoverySlotState::FAIL_CLOSED);
    }
    for (const auto stage : {recovery::RuntimeFailureStage::FINISH_COMPILE_INSTALL,
                             recovery::RuntimeFailureStage::SEAL_PUBLISH}) {
        ThrowAt failure{stage};
        auto in = compiling_coordinator(&failure);
        auto result = recovery::RecoveryCompiler::compile(
            *in.coordinator->active().current_snapshot, in.policy, in.registry, 0);
        if (stage == recovery::RuntimeFailureStage::FINISH_COMPILE_INSTALL) {
            in.coordinator->finish_compile(std::move(result));
            RECOVERY_CHECK(in.coordinator->active().compiled_program == nullptr);
        } else {
            failure.enabled = false;
            in.coordinator->finish_compile(std::move(result));
            auto installed = in.coordinator->active().compiled_program;
            failure.enabled = true;
            RECOVERY_CHECK(!in.coordinator->seal_validated_program(installed));
            RECOVERY_CHECK(in.coordinator->active().current_program == nullptr);
        }
        RECOVERY_CHECK_EQ(in.coordinator->state(), recovery::RecoverySlotState::FAIL_CLOSED);
    }
}

RECOVERY_TEST(runtime_failure_injection_before_freeze_publication_leaves_no_snapshot) {
    using Stage = recovery::RuntimeFailureStage;
    const Stage stages[] = {Stage::FREEZE_TARGETS, Stage::FREEZE_AFTER_BINDINGS,
                            Stage::FREEZE_SNAPSHOT};
    for (const auto stage : stages) {
        ThrowAt failure{stage};
        recovery::RecoveryCoordinator coordinator{11, nullptr, &failure};
        recovery::FaultReport f{recovery::FaultKind::PACKET_DOMAIN_FAULT,
                              {1, std::nullopt, std::nullopt}, {1,2,3,4}, {1,{}}};
        RECOVERY_CHECK(coordinator.report_fault(f));
        RECOVERY_CHECK(coordinator.begin_freeze({},
            {{1, recovery::ExecutorBindingStatus::RESOLVED,
              recovery::ExecutorPrincipal{recovery::ProcessPrincipal{{99, 100}}}, 1}}));
        RECOVERY_CHECK(!coordinator.commit_freeze());
        RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FAIL_CLOSED);
        RECOVERY_CHECK(!coordinator.active().current_snapshot.has_value());
        RECOVERY_CHECK(coordinator.active().process_exit_binding_registry.empty());
    }
}
}

RECOVERY_TEST(runtime_slot_has_single_optional_active_and_fail_closed_retains_it) {
    recovery::RecoveryCoordinator coordinator{11};
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::IDLE);
    RECOVERY_CHECK(!coordinator.has_active());
    recovery::FaultReport f{recovery::FaultKind::PACKET_DOMAIN_FAULT,
                          {1, std::nullopt, std::nullopt}, {1,2,3,4}, {1,{}}};
    RECOVERY_CHECK(coordinator.report_fault(f));
    RECOVERY_CHECK(coordinator.has_active());
    RECOVERY_CHECK(!coordinator.begin_compile());
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK(coordinator.has_active());
}

RECOVERY_TEST(R4_R6_R44_runtime_seal_revalidates_identity_and_frozen_inputs) {
    {
        auto in = compiling_coordinator();
        auto result = recovery::RecoveryCompiler::compile(
            *in.coordinator->active().current_snapshot, in.policy, in.registry, 0);
        in.coordinator->finish_compile(std::move(result));
        auto installed = in.coordinator->active().compiled_program;
        RECOVERY_CHECK(in.coordinator->seal_validated_program(installed));
        RECOVERY_CHECK_EQ(in.coordinator->state(), recovery::RecoverySlotState::SEALED);
    }
    {
        auto in = compiling_coordinator();
        auto first = recovery::RecoveryCompiler::compile(
            *in.coordinator->active().current_snapshot, in.policy, in.registry, 0);
        auto second = recovery::RecoveryCompiler::compile(
            *in.coordinator->active().current_snapshot, in.policy, in.registry, 0);
        in.coordinator->finish_compile(std::move(first));
        auto different = std::make_shared<const recovery::ValidatedRecoveryProgram>(
            std::get<recovery::ValidatedRecoveryProgram>(std::move(second)));
        RECOVERY_CHECK(!in.coordinator->seal_validated_program(different));
        RECOVERY_CHECK_EQ(in.coordinator->state(), recovery::RecoverySlotState::FAIL_CLOSED);
    }
    {
        auto in = compiling_coordinator();
        auto result = recovery::RecoveryCompiler::compile(
            *in.coordinator->active().current_snapshot, in.policy, in.registry, 0);
        in.coordinator->finish_compile(std::move(result));
        auto installed = in.coordinator->active().compiled_program;
        recovery::RecoveryRuntimeTestAccess::change_snapshot_digest(*in.coordinator);
        RECOVERY_CHECK(!in.coordinator->seal_validated_program(installed));
        RECOVERY_CHECK_EQ(in.coordinator->state(), recovery::RecoverySlotState::FAIL_CLOSED);
    }
}

RECOVERY_TEST(R14_new_scope_fault_after_seal_fails_closed) {
    auto in = compiling_coordinator();
    auto result = recovery::RecoveryCompiler::compile(
        *in.coordinator->active().current_snapshot, in.policy, in.registry, 0);
    in.coordinator->finish_compile(std::move(result));
    RECOVERY_CHECK(in.coordinator->seal_validated_program(
        in.coordinator->active().compiled_program));
    recovery::FaultReport expanded{recovery::FaultKind::RGA_DEVICE_FAULT,
        {2, std::nullopt, std::nullopt}, {1,2,3,4}, {2,{}}};
    RECOVERY_CHECK(!in.coordinator->report_fault(expanded));
    RECOVERY_CHECK_EQ(in.coordinator->state(), recovery::RecoverySlotState::FAIL_CLOSED);
}

RECOVERY_TEST(R6_policy_contents_mutation_is_rejected_at_seal) {
    auto in = compiling_coordinator();
    auto result = recovery::RecoveryCompiler::compile(
        *in.coordinator->active().current_snapshot, in.policy, in.registry, 0);
    in.coordinator->finish_compile(std::move(result));
    auto installed = in.coordinator->active().compiled_program;
    auto mutable_contents = std::const_pointer_cast<recovery::PolicyContents>(in.policy.contents);
    mutable_contents->dispositions[recovery::FaultKind::UNKNOWN] =
        recovery::FaultDisposition::COVERED;
    RECOVERY_CHECK(!in.coordinator->seal_validated_program(installed));
    RECOVERY_CHECK_EQ(in.coordinator->state(), recovery::RecoverySlotState::FAIL_CLOSED);
}

RECOVERY_TEST(R6_finish_compile_rejects_changed_frozen_policy) {
    auto in = compiling_coordinator();
    auto result = recovery::RecoveryCompiler::compile(
        *in.coordinator->active().current_snapshot, in.policy, in.registry, 0);
    recovery::RecoveryRuntimeTestAccess::change_policy_digest(*in.coordinator);
    in.coordinator->finish_compile(std::move(result));
    RECOVERY_CHECK_EQ(in.coordinator->state(), recovery::RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK(in.coordinator->active().compiled_program == nullptr);
}

RECOVERY_TEST(R4_R43_each_program_component_tamper_is_rejected_by_runtime_seal) {
    using Access = recovery::ValidatedRecoveryProgramTestAccess;
    const Access::Field fields[] = {Access::Field::HASH, Access::Field::FAULT,
                                    Access::Field::PLAN, Access::Field::PREDICATE,
                                    Access::Field::DAG};
    for (const auto field : fields) {
        auto in = compiling_coordinator();
        auto result = recovery::RecoveryCompiler::compile(
            *in.coordinator->active().current_snapshot, in.policy, in.registry, 0);
        auto& program = std::get<recovery::ValidatedRecoveryProgram>(result);
        Access::tamper(program, field);
        in.coordinator->finish_compile(std::move(result));
        auto installed = in.coordinator->active().compiled_program;
        RECOVERY_CHECK(installed != nullptr);
        RECOVERY_CHECK(!in.coordinator->seal_validated_program(installed));
        RECOVERY_CHECK_EQ(in.coordinator->state(), recovery::RecoverySlotState::FAIL_CLOSED);
    }
}

RECOVERY_TEST(R39_deleting_streamoff_or_reqbufs_zero_is_rejected_at_seal) {
    using Access = recovery::ValidatedRecoveryProgramTestAccess;
    for (const auto field : {Access::Field::STREAMOFF, Access::Field::REQBUFS_ZERO}) {
        auto in = v4l2_compiling_coordinator();
        auto result = recovery::RecoveryCompiler::compile(
            *in.coordinator->active().current_snapshot, in.policy, in.registry, 0);
        RECOVERY_CHECK(std::holds_alternative<recovery::ValidatedRecoveryProgram>(result));
        Access::tamper(std::get<recovery::ValidatedRecoveryProgram>(result), field);
        in.coordinator->finish_compile(std::move(result));
        RECOVERY_CHECK(!in.coordinator->seal_validated_program(
            in.coordinator->active().compiled_program));
        RECOVERY_CHECK_EQ(in.coordinator->state(), recovery::RecoverySlotState::FAIL_CLOSED);
    }
}
