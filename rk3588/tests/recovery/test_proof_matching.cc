#include <memory>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "recovery/proof.hpp"
#include "test_support.hpp"

namespace {

struct Inputs {
    recovery::FrozenRuntimeSnapshot snapshot;
    recovery::PolicySnapshot policy;
    recovery::CapabilityRegistrySnapshot registry;
};

Inputs inputs_for_process(recovery::ProcessIdentity process) {
    using namespace recovery;
    const FaultFact fault = RecoveryTestAccess::make_fault(
        7, 1, {FaultKind::PACKET_DOMAIN_FAULT,
               {42, std::nullopt, std::nullopt}, {3, 4, 5, 6}, {9, {}}});
    auto policy_contents = std::make_shared<PolicyContents>();
    policy_contents->dispositions.emplace(FaultKind::PACKET_DOMAIN_FAULT,
                                          FaultDisposition::COVERED);
    policy_contents->recipes.emplace(
        std::make_pair(ResourceKind::PACKET_DOMAIN, RecoveryMode::PACKET_RESET),
        CanonicalActionRecipe{
            ResourceKind::PACKET_DOMAIN, RecoveryMode::PACKET_RESET,
            {{ActionPhase::TEARDOWN, OperationKind::PACKET_STOP, {}},
             {ActionPhase::INVALIDATE_EPOCH, OperationKind::INVALIDATE_EPOCH, {0}},
             {ActionPhase::REINIT, OperationKind::REINIT, {1}}}});
    auto registry_contents = std::make_shared<CapabilityRegistryContents>();
    FrozenRuntimeSnapshot snapshot{};
    snapshot.recovery_id = {7, 1, 99};
    snapshot.freeze_seq = 2;
    snapshot.boot_id = 7;
    snapshot.fault_batch = {fault};
    snapshot.owner_executor_bindings = {
        {42, ExecutorBindingStatus::RESOLVED,
         ExecutorPrincipal{ProcessPrincipal{{123, 456}}}, 1}};
    snapshot.process_exit_bindings = {
        {{{7, 1, 99}, 2, 0}, process}};
    snapshot.epoch_snapshot = {3, 4, 5, 6};
    snapshot.ledger_prefix.push_back(
        FrozenGrantRecord{42, std::nullopt, {SubjectIdentity{process}}, {}});
    return {std::move(snapshot),
            PolicySnapshot{3, canonical_digest(*policy_contents), std::move(policy_contents)},
            CapabilityRegistrySnapshot{
                4, canonical_digest(*registry_contents), std::move(registry_contents)}};
}

recovery::ExecutionProofRecord process_record(
    const recovery::ValidatedRecoveryProgram& program,
    recovery::ProcessIdentity process,
    recovery::ProcessExitBindingId binding,
    recovery::WaitCode wait_code = recovery::WaitCode::EXITED,
    int wait_errno = 0) {
    return { {}, program.recovery_id(), program.transaction_hash(),
             recovery::PredicateKey{recovery::ExecutionPredicateKey{
                 recovery::ExecutionProofKind::PROCESS_EXIT_CONFIRMED,
                 recovery::SubjectIdentity{process}}},
             recovery::ExecutionProofKind::PROCESS_EXIT_CONFIRMED,
             recovery::ProofIssuer{0, recovery::IssuerRole::EXECUTION_AGENT},
             recovery::SubjectIdentity{process}, {3, 4, 5, 6},
             recovery::ExecutionProofResult{recovery::ProcessExitResult{
                 process, binding, process.start_generation, wait_code,
                 wait_errno, true}}, 0};
}

}  // namespace

RECOVERY_TEST(R46_successful_process_exit_requires_trusted_observation) {
    using namespace recovery;
    const pid_t child = ::fork();
    RECOVERY_CHECK(child >= 0);
    if (child == 0) ::_exit(0);
    const ProcessIdentity process{static_cast<ProcessId>(child), 77};
    auto inputs = inputs_for_process(process);
    const auto compiled = RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, inputs.registry, 100);
    RECOVERY_CHECK(std::holds_alternative<ValidatedRecoveryProgram>(compiled));
    const auto& program = std::get<ValidatedRecoveryProgram>(compiled);
    const auto id = program.execution_predicates().front().expected_process_binding;
    RECOVERY_CHECK(id.has_value());
    const int pidfd = static_cast<int>(::syscall(SYS_pidfd_open, child, 0));
    RECOVERY_CHECK(pidfd >= 0);
    ProcessExitBindingRegistry bindings;
    bindings.emplace(*id, OwnedProcessExitBinding{
        ProcessExitBindingDescriptor{*id, process}, OwnedFd{pidfd}});

    std::optional<TrustedProcessExitObservation> observed;
    for (int attempt = 0; attempt != 100 && (!observed || !observed->exited); ++attempt) {
        observed = observe_process_exit_trusted(bindings.begin()->second);
        if (!observed->exited) ::usleep(1000);
    }
    RECOVERY_CHECK(observed.has_value() && observed->exited);
    auto record = process_record(program, process, *id, observed->wait_code,
                                 observed->wait_errno);
    RECOVERY_CHECK(execution_proof_matches(
        record, program.execution_predicates().front(), program,
        VerificationContext{100, inputs.registry}, bindings));
    int status = 0;
    RECOVERY_CHECK(::waitpid(child, &status, 0) == child);
}

RECOVERY_TEST(R46_declared_success_is_rejected_when_trusted_identity_differs) {
    using namespace recovery;
    const ProcessIdentity expected{101, 77};
    const auto inputs = inputs_for_process(expected);
    const auto compiled = RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, inputs.registry, 100);
    RECOVERY_CHECK(std::holds_alternative<ValidatedRecoveryProgram>(compiled));
    const auto& program = std::get<ValidatedRecoveryProgram>(compiled);
    const auto id = program.execution_predicates().front().expected_process_binding;
    RECOVERY_CHECK(id.has_value());
    ProcessExitBindingRegistry bindings;
    bindings.emplace(*id, OwnedProcessExitBinding{
        ProcessExitBindingDescriptor{*id, expected}, OwnedFd{}});
    const auto record = process_record(program, expected, *id);
    RECOVERY_CHECK(!execution_proof_matches(
        record, program.execution_predicates().front(), program,
        VerificationContext{100, inputs.registry}, bindings));

    auto cross_recovery = record;
    cross_recovery.recovery_id = {8, 9, 10};
    RECOVERY_CHECK(!execution_proof_matches(
        cross_recovery, program.execution_predicates().front(), program,
        VerificationContext{100, inputs.registry}, bindings));
}

RECOVERY_TEST(R46_registry_key_must_equal_owned_descriptor_id) {
    using namespace recovery;
    const ProcessIdentity process{101, 77};
    const auto inputs = inputs_for_process(process);
    const auto compiled = RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, inputs.registry, 100);
    RECOVERY_CHECK(std::holds_alternative<ValidatedRecoveryProgram>(compiled));
    const auto& program = std::get<ValidatedRecoveryProgram>(compiled);
    const auto expected = program.execution_predicates().front().expected_process_binding;
    RECOVERY_CHECK(expected.has_value());
    auto wrong = *expected;
    wrong.binding_seq++;
    ProcessExitBindingRegistry bindings;
    bindings.emplace(*expected, OwnedProcessExitBinding{
        ProcessExitBindingDescriptor{wrong, process}, OwnedFd{}});
    const auto record = process_record(program, process, *expected);
    RECOVERY_CHECK(!execution_proof_matches(
        record, program.execution_predicates().front(), program,
        VerificationContext{100, inputs.registry}, bindings));
}

RECOVERY_TEST(R46_declared_wait_and_generation_must_match_observation) {
    using namespace recovery;
    const pid_t child = ::fork();
    RECOVERY_CHECK(child >= 0);
    if (child == 0) ::_exit(0);
    const ProcessIdentity process{static_cast<ProcessId>(child), 77};
    auto inputs = inputs_for_process(process);
    const auto compiled = RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, inputs.registry, 100);
    RECOVERY_CHECK(std::holds_alternative<ValidatedRecoveryProgram>(compiled));
    const auto& program = std::get<ValidatedRecoveryProgram>(compiled);
    const auto id = program.execution_predicates().front().expected_process_binding;
    RECOVERY_CHECK(id.has_value());
    ProcessExitBindingRegistry bindings;
    bindings.emplace(*id, OwnedProcessExitBinding{
        ProcessExitBindingDescriptor{*id, process},
        OwnedFd{static_cast<int>(::syscall(SYS_pidfd_open, child, 0))}});
    std::optional<TrustedProcessExitObservation> observed;
    for (int attempt = 0; attempt != 100 && (!observed || !observed->exited); ++attempt) {
        observed = observe_process_exit_trusted(bindings.begin()->second);
        if (!observed->exited) ::usleep(1000);
    }
    RECOVERY_CHECK(observed.has_value() && observed->exited);
    auto wrong_wait = process_record(program, process, *id,
                                     observed->wait_code, observed->wait_errno);
    wrong_wait.result = ProcessExitResult{
        process, *id, process.start_generation + 1, observed->wait_code,
        observed->wait_errno, true};
    RECOVERY_CHECK(!execution_proof_matches(
        wrong_wait, program.execution_predicates().front(), program,
        VerificationContext{100, inputs.registry}, bindings));
    int status = 0;
    RECOVERY_CHECK(::waitpid(child, &status, 0) == child);
}

RECOVERY_TEST(R45_thread_join_result_identity_is_checked) {
    using namespace recovery;
    const ProcessIdentity process{101, 77};
    const auto inputs = inputs_for_process(process);
    const auto compiled = RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, inputs.registry, 100);
    RECOVERY_CHECK(std::holds_alternative<ValidatedRecoveryProgram>(compiled));
    const auto& program = std::get<ValidatedRecoveryProgram>(compiled);
    const ThreadIdentity expected{101, 4, 77};
    const ThreadIdentity wrong{101, 5, 77};
    const ExecutionPredicate predicate{
        PredicateKey{ExecutionPredicateKey{ExecutionProofKind::THREADS_JOINED,
                                           SubjectIdentity{expected}}},
        ExecutionProofKind::THREADS_JOINED,
        ProofIssuer{0, IssuerRole::EXECUTION_AGENT},
        SubjectIdentity{expected}, 0, std::nullopt, std::nullopt};
    const ExecutionProofRecord record{
        {}, program.recovery_id(), program.transaction_hash(), predicate.key,
        predicate.required_kind, predicate.required_issuer, predicate.expected_subject,
        {}, ExecutionProofResult{ThreadJoinResult{wrong, 0, true}}, 0};
    RECOVERY_CHECK(!execution_proof_matches(
        record, predicate, program, VerificationContext{100, inputs.registry}, {}));
}

RECOVERY_TEST(R23_R24_registry_identity_and_evidence_validity_are_rechecked) {
    using namespace recovery;
    const ProcessIdentity process{101, 77};
    const auto inputs = inputs_for_process(process);
    auto registry_contents = std::make_shared<CapabilityRegistryContents>();
    CapabilityEvidence evidence{
        5, 77, {{1}, {2}, {3}, {4}, 0}, 12, {8}, 50, 150};
    registry_contents->evidences.push_back(evidence);
    CapabilityExpectation expectation{
        evidence.capability_id, evidence.device_context, evidence.environment,
        evidence.evidence_id, evidence.evidence_digest, evidence.valid_from,
        evidence.valid_until};
    const CapabilityRegistrySnapshot registry{
        4, canonical_digest(*registry_contents), registry_contents};
    auto program_result = RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, registry, 100);
    RECOVERY_CHECK(std::holds_alternative<ValidatedRecoveryProgram>(program_result));
    const auto& program = std::get<ValidatedRecoveryProgram>(program_result);
    RECOVERY_CHECK(capability_evidence_match(
        evidence, expectation, program, VerificationContext{100, registry}));
    RECOVERY_CHECK(!capability_evidence_match(
        evidence, expectation, program,
        VerificationContext{100, CapabilityRegistrySnapshot{
            5, registry.digest, registry_contents}}));
    auto revoked_contents = std::make_shared<CapabilityRegistryContents>(*registry_contents);
    revoked_contents->revoked_evidence.insert(evidence.evidence_id);
    const CapabilityRegistrySnapshot revoked{
        4, canonical_digest(*revoked_contents), std::move(revoked_contents)};
    RECOVERY_CHECK(!capability_evidence_match(
        evidence, expectation, program, VerificationContext{100, revoked}));
}

RECOVERY_TEST(R48_R49_device_result_identity_and_reset_generation_are_checked) {
    using namespace recovery;
    const ProcessIdentity process{101, 77};
    auto inputs = inputs_for_process(process);
    constexpr DeviceContextIdentity device = 77;
    const CapabilityEvidence evidence{
        5, device, {{1}, {2}, {3}, {4}, 0}, 12, {8}, 50, 150};
    auto policy = std::make_shared<PolicyContents>(*inputs.policy.contents);
    policy->device_capability_map.emplace(device, evidence.capability_id);
    policy->resource_device_proof_kind.emplace(
        ResourceKind::PACKET_DOMAIN, DeviceProofKind::CONTEXT_DESTROY_CONFIRMED);
    inputs.policy.contents = std::move(policy);
    inputs.policy.digest = canonical_digest(*inputs.policy.contents);
    inputs.snapshot.fault_batch.front().subject.device_context = device;
    inputs.snapshot.ledger_prefix.push_back(
        FrozenGrantRecord{42, std::nullopt, {}, {{device}}});
    auto registry_contents = std::make_shared<CapabilityRegistryContents>();
    registry_contents->evidences.push_back(evidence);
    inputs.registry.contents = std::move(registry_contents);
    inputs.registry.digest = canonical_digest(*inputs.registry.contents);

    const auto compiled = RecoveryCompiler::compile(
        inputs.snapshot, inputs.policy, inputs.registry, 100);
    RECOVERY_CHECK(std::holds_alternative<ValidatedRecoveryProgram>(compiled));
    const auto& program = std::get<ValidatedRecoveryProgram>(compiled);
    const auto& context_predicate = program.device_predicates().front();
    const auto context_record = DeviceProofRecord{
        {}, program.recovery_id(), program.transaction_hash(), context_predicate.key,
        context_predicate.required_kind, context_predicate.required_issuer,
        context_predicate.expected_target, {3, 4, 5, 6}, evidence,
        DeviceProofResult{ContextTeardownResult{device, true, true, evidence}}, 0};
    RECOVERY_CHECK(device_proof_matches(
        context_record, context_predicate, program,
        VerificationContext{100, inputs.registry}));
    auto wrong_context = context_record;
    wrong_context.result = ContextTeardownResult{device + 1, true, true, evidence};
    RECOVERY_CHECK(!device_proof_matches(
        wrong_context, context_predicate, program,
        VerificationContext{100, inputs.registry}));

    const DevicePredicate reset_predicate{
        PredicateKey{DevicePredicateKey{DeviceProofKind::HARDWARE_RESET_CONFIRMED, device}},
        DeviceProofKind::HARDWARE_RESET_CONFIRMED,
        ProofIssuer{device, IssuerRole::DEVICE_DRIVER}, device, DeviceTarget{device}, 0,
        context_predicate.expected_capability};
    const DeviceProofRecord bad_reset{
        {}, program.recovery_id(), program.transaction_hash(), reset_predicate.key,
        reset_predicate.required_kind, reset_predicate.required_issuer,
        reset_predicate.expected_target, {3, 4, 5, 6}, evidence,
        DeviceProofResult{HardwareResetResult{1, device, 8, 8, true, true, evidence}}, 0};
    RECOVERY_CHECK(!device_proof_matches(
        bad_reset, reset_predicate, program,
        VerificationContext{100, inputs.registry}));
}
