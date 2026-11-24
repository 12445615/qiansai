#include <cerrno>
#include <poll.h>
#include <sys/wait.h>

namespace {
constexpr int kEchildError = ECHILD;
}

#include "recovery/proof.hpp"

#include <algorithm>
#include <set>
#include <type_traits>

namespace recovery {
namespace {

#ifndef P_PIDFD
constexpr idtype_t kPidfdIdType = static_cast<idtype_t>(3);
#else
constexpr idtype_t kPidfdIdType = P_PIDFD;
#endif

bool epoch_matches(const EpochSnapshot& epochs, EpochMask mask) noexcept {
    if ((mask & (1U << static_cast<unsigned>(EpochDomain::STREAM)))
        && epochs.stream_epoch == 0) return false;
    if ((mask & (1U << static_cast<unsigned>(EpochDomain::AI)))
        && epochs.ai_epoch == 0) return false;
    if ((mask & (1U << static_cast<unsigned>(EpochDomain::WORKER)))
        && epochs.worker_epoch == 0) return false;
    if ((mask & (1U << static_cast<unsigned>(EpochDomain::MEDIA)))
        && epochs.media_epoch == 0) return false;
    return true;
}

bool evidence_in_registry(const CapabilityEvidence& evidence,
                          const CapabilityRegistryContents& contents) noexcept {
    return std::any_of(contents.evidences.begin(), contents.evidences.end(),
                       [&evidence](const auto& candidate) {
                           return candidate == evidence;
                       });
}

const ExecutionPredicate* execution_predicate_of(
    const ValidatedRecoveryProgram& program, const PredicateKey& key) noexcept {
    for (const auto& predicate : program.execution_predicates()) {
        if (predicate.key == key) return &predicate;
    }
    return nullptr;
}

const DevicePredicate* device_predicate_of(
    const ValidatedRecoveryProgram& program, const PredicateKey& key) noexcept {
    for (const auto& predicate : program.device_predicates()) {
        if (predicate.key == key) return &predicate;
    }
    return nullptr;
}

}  // namespace

std::optional<TrustedProcessExitObservation>
observe_process_exit_trusted(const OwnedProcessExitBinding& owned) noexcept {
    TrustedProcessExitObservation observation{
        owned.descriptor.id, owned.descriptor.process, WaitCode::UNKNOWN, 0, false,
        owned.descriptor.process.start_generation};
    if (!owned.pidfd.valid()) return std::nullopt;

    pollfd readiness{owned.pidfd.get(), POLLIN, 0};
    const int poll_result = ::poll(&readiness, 1, 0);
    if (poll_result < 0) {
        observation.wait_errno = errno;
        return observation;
    }
    if (poll_result == 0) return observation;

    siginfo_t info{};
    if (::waitid(kPidfdIdType, static_cast<id_t>(owned.pidfd.get()), &info,
                 WEXITED | WNOHANG | WNOWAIT) != 0) {
        if (errno == kEchildError) observation.wait_code = WaitCode::ECHILD;
        else observation.wait_errno = errno;
        return observation;
    }
    if (info.si_pid == 0) return observation;
    if (static_cast<ProcessId>(info.si_pid) != owned.descriptor.process.pid) {
        observation.wait_errno = ESRCH;
        return observation;
    }
    if (info.si_code == CLD_EXITED) {
        observation.wait_code = WaitCode::EXITED;
        observation.exited = true;
    } else if (info.si_code == CLD_KILLED || info.si_code == CLD_DUMPED) {
        observation.wait_code = WaitCode::SIGNALED;
        observation.exited = true;
    }
    return observation;
}

bool capability_evidence_match(
    const CapabilityEvidence& evidence, const CapabilityExpectation& expected,
    const ValidatedRecoveryProgram& program, const VerificationContext& context) noexcept {
    if (!context.registry.contents
        || canonical_digest(*context.registry.contents) != context.registry.digest) return false;
    if (context.registry.version != program.registry_version()
        || context.registry.digest != program.registry_digest()) return false;
    if (!evidence_in_registry(evidence, *context.registry.contents)) return false;
    if (context.registry.contents->revoked_evidence.contains(evidence.evidence_id)) return false;
    return evidence.capability_id == expected.capability_id
        && evidence.device_context == expected.device_context
        && evidence.environment == expected.environment
        && evidence.evidence_id == expected.evidence_id
        && evidence.evidence_digest == expected.evidence_digest
        && evidence.valid_from == expected.valid_from
        && evidence.valid_until == expected.valid_until
        && context.now >= evidence.valid_from
        && context.now <= evidence.valid_until;
}

bool execution_proof_matches(
    const ExecutionProofRecord& record, const ExecutionPredicate& predicate,
    const ValidatedRecoveryProgram& program, const VerificationContext& context,
    const ProcessExitBindingRegistry& trusted_bindings) noexcept {
    (void)context;
    if (record.recovery_id != program.recovery_id()
        || record.transaction_hash != program.transaction_hash()) return false;
    if (record.kind != predicate.required_kind
        || record.issuer != predicate.required_issuer
        || record.subject != predicate.expected_subject
        || !epoch_matches(record.epochs, predicate.epoch_mask)) return false;

    if (record.kind == ExecutionProofKind::THREADS_JOINED) {
        const auto* result = std::get_if<ThreadJoinResult>(&record.result);
        const auto* subject = std::get_if<ThreadIdentity>(&record.subject);
        const auto* expected = std::get_if<ThreadIdentity>(&predicate.expected_subject);
        return result && subject && expected && result->thread == *subject
            && result->thread == *expected && result->join_rc == 0 && result->terminated;
    }

    if (record.kind != ExecutionProofKind::PROCESS_EXIT_CONFIRMED) return false;
    const auto* result = std::get_if<ProcessExitResult>(&record.result);
    const auto* subject = std::get_if<ProcessIdentity>(&record.subject);
    const auto* expected = std::get_if<ProcessIdentity>(&predicate.expected_subject);
    if (!result || !subject || !expected || result->process != *subject
        || result->process != *expected || !predicate.expected_process_binding
        || result->binding_id != *predicate.expected_process_binding) return false;

    const auto found = trusted_bindings.find(*predicate.expected_process_binding);
    if (found == trusted_bindings.end()) return false;
    const auto& owned = found->second;
    if (found->first != owned.descriptor.id
        || owned.descriptor.process != *expected || !owned.pidfd.valid()) return false;

    const auto observed = observe_process_exit_trusted(owned);
    if (!observed || observed->binding_id != found->first
        || observed->process != owned.descriptor.process || !observed->exited
        || (observed->wait_code != WaitCode::EXITED
            && observed->wait_code != WaitCode::SIGNALED)
        || observed->wait_errno != 0) return false;
    if (result->wait_code != observed->wait_code
        || result->wait_errno != observed->wait_errno
        || result->spawn_generation != observed->spawn_generation
        || !predicate.expected_spawn_generation
        || result->spawn_generation != *predicate.expected_spawn_generation) return false;
    return result->terminated_confirmed;
}

bool device_proof_matches(
    const DeviceProofRecord& record, const DevicePredicate& predicate,
    const ValidatedRecoveryProgram& program, const VerificationContext& context) noexcept {
    if (record.recovery_id != program.recovery_id()
        || record.transaction_hash != program.transaction_hash()
        || record.kind != predicate.required_kind
        || record.issuer != predicate.required_issuer
        || record.device_target != predicate.expected_target
        || record.device_target.device_context != predicate.expected_device_context
        || !epoch_matches(record.epochs, predicate.epoch_mask)
        || !capability_evidence_match(record.evidence, predicate.expected_capability,
                                       program, context)) return false;

    if (record.kind == DeviceProofKind::DRIVER_FENCE_SIGNALED) {
        const auto* result = std::get_if<FenceResult>(&record.result);
        return result && result->device_context == record.device_target.device_context
            && (result->fence >> 32) == result->device_context
            && result->signaled && result->fence_errno == 0;
    }
    if (record.kind == DeviceProofKind::CONTEXT_DESTROY_CONFIRMED) {
        const auto* result = std::get_if<ContextTeardownResult>(&record.result);
        return result && result->device_context == record.device_target.device_context
            && result->teardown_call_succeeded && result->verified
            && result->evidence == record.evidence;
    }
    if (record.kind == DeviceProofKind::HARDWARE_RESET_CONFIRMED) {
        const auto* result = std::get_if<HardwareResetResult>(&record.result);
        return result && result->device_context == record.device_target.device_context
            && result->evidence == record.evidence
            && result->new_reset_generation > result->old_reset_generation
            && result->post_reset_idle && result->post_reset_ready;
    }
    return false;
}

bool proof_matches(
    const ProofRecord& record, const ValidatedRecoveryProgram& program,
    const VerificationContext& context,
    const ProcessExitBindingRegistry& trusted_bindings) noexcept {
    return std::visit([&](const auto& typed) {
        using Record = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Record, ExecutionProofRecord>) {
            const auto* predicate = execution_predicate_of(program, typed.key);
            return predicate && execution_proof_matches(
                typed, *predicate, program, context, trusted_bindings);
        } else {
            const auto* predicate = device_predicate_of(program, typed.key);
            return predicate && device_proof_matches(typed, *predicate, program, context);
        }
    }, record);
}

bool proof_record_matches_key(const ProofRecord& record,
                              const PredicateKey& key) noexcept {
    return std::visit([&](const auto& typed) {
        using Record = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Record, ExecutionProofRecord>) {
            const auto* execution = std::get_if<ExecutionPredicateKey>(&key);
            return execution && execution->kind == typed.kind
                && execution->subject == typed.subject;
        } else {
            const auto* device = std::get_if<DevicePredicateKey>(&key);
            return device && device->kind == typed.kind
                && device->device_context == typed.device_target.device_context;
        }
    }, record);
}

bool barrier_complete(const ValidatedRecoveryProgram& program,
                      const ProofIndex& proofs) noexcept {
    std::set<PredicateKey> required;
    for (const auto& predicate : program.execution_predicates())
        required.insert(predicate.key);
    for (const auto& predicate : program.device_predicates())
        required.insert(predicate.key);
    std::set<PredicateKey> present;
    for (const auto& [key, record] : proofs.by_key) {
        present.insert(key);
        if (!proof_record_matches_key(record, key)) return false;
    }
    return present == required;
}

ProofRecordHash proof_hash_of(const ProofRecord& record) noexcept {
    return std::visit([](const auto& typed) { return typed.proof_hash; }, record);
}

ProofRecordHash compute_proof_hash(const ProofRecord& record) {
    return std::visit([](const auto& typed) {
        auto copy = typed;
        copy.proof_hash = {};
        return canonical_digest(copy);
    }, record);
}

}  // namespace recovery
