#ifndef RECOVERY_PROOF_HPP
#define RECOVERY_PROOF_HPP

#include "recovery/canonical.hpp"
#include "recovery/runtime.hpp"

#include <map>
#include <optional>
#include <variant>

namespace recovery {

using ProofRecordHash = Digest;

#ifdef ECHILD
#undef ECHILD
#endif
enum class WaitCode : std::uint32_t { EXITED, SIGNALED, ECHILD, UNKNOWN };

struct ThreadJoinResult {
    ThreadIdentity thread;
    int join_rc;
    bool terminated;
    auto operator<=>(const ThreadJoinResult&) const = default;
};

struct ProcessExitResult {
    ProcessIdentity process;
    ProcessExitBindingId binding_id;
    std::uint64_t spawn_generation;
    WaitCode wait_code;
    int wait_errno;
    bool terminated_confirmed;
    auto operator<=>(const ProcessExitResult&) const = default;
};

struct FenceResult {
    DeviceContextIdentity device_context;
    std::uint64_t fence;
    bool signaled;
    int fence_errno;
    EpochSnapshot epochs;
    auto operator<=>(const FenceResult&) const = default;
};

struct ContextTeardownResult {
    DeviceContextIdentity device_context;
    bool teardown_call_succeeded;
    bool verified;
    CapabilityEvidence evidence;
    auto operator<=>(const ContextTeardownResult&) const = default;
};

struct HardwareResetResult {
    std::uint64_t reset_domain;
    DeviceContextIdentity device_context;
    std::uint64_t old_reset_generation;
    std::uint64_t new_reset_generation;
    bool post_reset_idle;
    bool post_reset_ready;
    CapabilityEvidence evidence;
    auto operator<=>(const HardwareResetResult&) const = default;
};

using ExecutionProofResult = std::variant<ThreadJoinResult, ProcessExitResult>;
using DeviceProofResult = std::variant<FenceResult, ContextTeardownResult,
                                        HardwareResetResult>;

struct ExecutionProofRecord {
    ProofRecordHash proof_hash{};
    RecoveryInstanceId recovery_id;
    TransactionHash transaction_hash;
    PredicateKey key;
    ExecutionProofKind kind;
    ProofIssuer issuer;
    SubjectIdentity subject;
    EpochSnapshot epochs;
    ExecutionProofResult result;
    TrustedMonotonicTime trusted_timestamp;
};

struct DeviceProofRecord {
    ProofRecordHash proof_hash{};
    RecoveryInstanceId recovery_id;
    TransactionHash transaction_hash;
    PredicateKey key;
    DeviceProofKind kind;
    ProofIssuer issuer;
    DeviceTarget device_target;
    EpochSnapshot epochs;
    CapabilityEvidence evidence;
    DeviceProofResult result;
    TrustedMonotonicTime trusted_timestamp;
};

using ProofRecord = std::variant<ExecutionProofRecord, DeviceProofRecord>;

struct TrustedProcessExitObservation {
    ProcessExitBindingId binding_id;
    ProcessIdentity process;
    WaitCode wait_code;
    int wait_errno;
    bool exited;
    std::uint64_t spawn_generation;
};

struct VerificationContext {
    TrustedMonotonicTime now;
    CapabilityRegistrySnapshot registry;

    VerificationContext(TrustedMonotonicTime trusted_now,
                        CapabilityRegistrySnapshot trusted_registry)
        : now(trusted_now), registry(std::move(trusted_registry)) {}
};

struct ProofIndex {
    std::map<PredicateKey, ProofRecord> by_key;
};

[[nodiscard]] std::optional<TrustedProcessExitObservation>
observe_process_exit_trusted(const OwnedProcessExitBinding&) noexcept;

[[nodiscard]] bool capability_evidence_match(
    const CapabilityEvidence&, const CapabilityExpectation&,
    const ValidatedRecoveryProgram&, const VerificationContext&) noexcept;

[[nodiscard]] bool execution_proof_matches(
    const ExecutionProofRecord&, const ExecutionPredicate&,
    const ValidatedRecoveryProgram&, const VerificationContext&,
    const ProcessExitBindingRegistry&) noexcept;

[[nodiscard]] bool device_proof_matches(
    const DeviceProofRecord&, const DevicePredicate&,
    const ValidatedRecoveryProgram&, const VerificationContext&) noexcept;

[[nodiscard]] bool proof_matches(
    const ProofRecord&, const ValidatedRecoveryProgram&,
    const VerificationContext&, const ProcessExitBindingRegistry&) noexcept;

[[nodiscard]] bool proof_record_matches_key(
    const ProofRecord&, const PredicateKey&) noexcept;

[[nodiscard]] bool barrier_complete(
    const ValidatedRecoveryProgram&, const ProofIndex&) noexcept;

[[nodiscard]] ProofRecordHash compute_proof_hash(const ProofRecord&);
[[nodiscard]] ProofRecordHash proof_hash_of(const ProofRecord&) noexcept;

inline void canonical_write(CanonicalWriter& writer, WaitCode value) {
    canonical_write(writer, static_cast<std::uint32_t>(value));
}

inline void canonical_write(CanonicalWriter& writer, const ThreadJoinResult& value) {
    canonical_write(writer, value.thread);
    canonical_write(writer, value.join_rc);
    canonical_write(writer, value.terminated);
}

inline void canonical_write(CanonicalWriter& writer, const ProcessExitResult& value) {
    canonical_write(writer, value.process);
    canonical_write(writer, value.binding_id);
    canonical_write(writer, value.spawn_generation);
    canonical_write(writer, value.wait_code);
    canonical_write(writer, value.wait_errno);
    canonical_write(writer, value.terminated_confirmed);
}

inline void canonical_write(CanonicalWriter& writer, const FenceResult& value) {
    canonical_write(writer, value.device_context);
    canonical_write(writer, value.fence);
    canonical_write(writer, value.signaled);
    canonical_write(writer, value.fence_errno);
    canonical_write(writer, value.epochs);
}

inline void canonical_write(CanonicalWriter& writer, const ContextTeardownResult& value) {
    canonical_write(writer, value.device_context);
    canonical_write(writer, value.teardown_call_succeeded);
    canonical_write(writer, value.verified);
    canonical_write(writer, value.evidence);
}

inline void canonical_write(CanonicalWriter& writer, const HardwareResetResult& value) {
    canonical_write(writer, value.reset_domain);
    canonical_write(writer, value.device_context);
    canonical_write(writer, value.old_reset_generation);
    canonical_write(writer, value.new_reset_generation);
    canonical_write(writer, value.post_reset_idle);
    canonical_write(writer, value.post_reset_ready);
    canonical_write(writer, value.evidence);
}

inline void canonical_write(CanonicalWriter& writer, const ExecutionProofRecord& value) {
    canonical_write(writer, value.proof_hash);
    canonical_write(writer, value.recovery_id);
    canonical_write(writer, value.transaction_hash);
    canonical_write(writer, value.key);
    canonical_write(writer, value.kind);
    canonical_write(writer, value.issuer);
    canonical_write(writer, value.subject);
    canonical_write(writer, value.epochs);
    canonical_write(writer, value.result);
    canonical_write(writer, value.trusted_timestamp);
}

inline void canonical_write(CanonicalWriter& writer, const DeviceProofRecord& value) {
    canonical_write(writer, value.proof_hash);
    canonical_write(writer, value.recovery_id);
    canonical_write(writer, value.transaction_hash);
    canonical_write(writer, value.key);
    canonical_write(writer, value.kind);
    canonical_write(writer, value.issuer);
    canonical_write(writer, value.device_target);
    canonical_write(writer, value.epochs);
    canonical_write(writer, value.evidence);
    canonical_write(writer, value.result);
    canonical_write(writer, value.trusted_timestamp);
}

}  // namespace recovery

#endif
