#include "recovery/proof.hpp"
#include "test_support.hpp"

#include <memory>

namespace {

recovery::ValidatedRecoveryProgram program_with_process_predicate() {
    using namespace recovery;
    const ProcessIdentity process{101, 77};
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
    auto result = RecoveryCompiler::compile(
        snapshot, PolicySnapshot{3, canonical_digest(*policy_contents), policy_contents},
        CapabilityRegistrySnapshot{4, canonical_digest(*registry_contents), registry_contents},
        100);
    RECOVERY_CHECK(std::holds_alternative<ValidatedRecoveryProgram>(result));
    return std::move(std::get<ValidatedRecoveryProgram>(result));
}

recovery::ExecutionProofRecord record_for(
    const recovery::ValidatedRecoveryProgram& program) {
    using namespace recovery;
    const auto& predicate = program.execution_predicates().front();
    const auto process = std::get<ProcessIdentity>(predicate.expected_subject);
    return { {}, program.recovery_id(), program.transaction_hash(), predicate.key,
             predicate.required_kind, predicate.required_issuer, predicate.expected_subject,
             {3, 4, 5, 6}, ExecutionProofResult{ProcessExitResult{
                 process, *predicate.expected_process_binding, process.start_generation,
                 WaitCode::EXITED, 0, true}}, 0};
}

}  // namespace

RECOVERY_TEST(R20_R21_R22_barrier_requires_exact_typed_predicate_set) {
    using namespace recovery;
    auto program = program_with_process_predicate();
    ProofIndex proofs;
    const auto& predicate = program.execution_predicates().front();
    RECOVERY_CHECK(!barrier_complete(program, proofs));

    const auto record = record_for(program);
    auto hashed = record;
    hashed.proof_hash = compute_proof_hash(ProofRecord{hashed});
    RECOVERY_CHECK_EQ(hashed.proof_hash, compute_proof_hash(ProofRecord{hashed}));
    auto changed = hashed;
    changed.trusted_timestamp++;
    RECOVERY_CHECK(changed.proof_hash != compute_proof_hash(ProofRecord{changed}));
    const auto first_insert = proofs.by_key.emplace(predicate.key, ProofRecord{record});
    const auto second_insert = proofs.by_key.emplace(predicate.key, ProofRecord{record});
    RECOVERY_CHECK(first_insert.second);
    RECOVERY_CHECK(!second_insert.second);
    RECOVERY_CHECK(barrier_complete(program, proofs));

    proofs.by_key.clear();
    DeviceProofRecord wrong_variant{
        {}, program.recovery_id(), program.transaction_hash(), predicate.key,
        DeviceProofKind::CONTEXT_DESTROY_CONFIRMED,
        ProofIssuer{0, IssuerRole::DEVICE_DRIVER}, DeviceTarget{0}, {3, 4, 5, 6},
        {}, DeviceProofResult{FenceResult{0, 0, true, 0, {3, 4, 5, 6}}}, 0};
    proofs.by_key.emplace(predicate.key, ProofRecord{wrong_variant});
    RECOVERY_CHECK(!barrier_complete(program, proofs));
}
