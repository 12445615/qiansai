#ifndef RECOVERY_PROGRAM_HPP
#define RECOVERY_PROGRAM_HPP

#include "recovery/crypto.hpp"

#include <compare>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <variant>
#include <vector>

namespace recovery {

class RecoveryCoordinator;
#ifdef RECOVERY_TESTING
struct RecoveryTestAccess;
#endif

using PolicyVersion = std::uint64_t;
using CapabilityRegistryVersion = std::uint64_t;
using TrustedMonotonicTime = std::uint64_t;
using ResourceInstance = std::uint64_t;
using EvidenceId = std::uint64_t;
using CapabilityId = std::uint64_t;
using EvidenceDigest = Digest;
using EpochMask = std::uint32_t;

enum class EpochDomain : std::uint32_t { STREAM, AI, WORKER, MEDIA };
struct EpochSnapshot {
    std::uint64_t stream_epoch, ai_epoch, worker_epoch, media_epoch;
    auto operator<=>(const EpochSnapshot&) const = default;
};
enum class ResourceKind : std::uint32_t {
    V4L2_CAPTURE, RKNN_AI, RGA, MPP_ENCODER, WORKER_PROCESS, PACKET_DOMAIN,
    RTMP_SINK, RECORD_SINK
};
enum class RecoveryMode : std::uint32_t {
    CAPTURE_RESET, AI_RESET, ENCODING_RESET, WORKER_RESTART, PACKET_RESET,
    RTMP_SINK_RESET, RECORD_SINK_RESET
};
enum class FaultKind : std::uint32_t {
    V4L2_DEVICE_FAULT, RGA_DEVICE_FAULT, RKNN_DEVICE_FAULT, MPP_DEVICE_FAULT,
    PACKET_DOMAIN_FAULT, RTMP_SINK_FAULT, RECORD_SINK_FAULT,
    WORKER_PROCESS_EXIT, CAPABILITY_FAULT, UNKNOWN
};
struct FaultSubject {
    ResourceInstance resource;
    std::optional<DeviceContextIdentity> device_context;
    std::optional<ProcessIdentity> process;
    auto operator<=>(const FaultSubject&) const = default;
};
struct FaultEvidence {
    EvidenceId evidence_id;
    Digest evidence_digest;
    auto operator<=>(const FaultEvidence&) const = default;
};
struct FaultReport {
    FaultKind kind;
    FaultSubject subject;
    EpochSnapshot observed_epochs;
    FaultEvidence evidence;
    auto operator<=>(const FaultReport&) const = default;
};
struct FaultIdentity {
    BootId boot_id;
    std::uint64_t fault_seq;
    auto operator<=>(const FaultIdentity&) const = default;
private:
    FaultIdentity(BootId boot, std::uint64_t sequence)
        : boot_id(boot), fault_seq(sequence) {}
    friend class RecoveryCoordinator;
#ifdef RECOVERY_TESTING
    friend struct RecoveryTestAccess;
#endif
};
struct FaultFact {
    FaultIdentity id;
    FaultKind kind;
    FaultSubject subject;
    EpochSnapshot observed_epochs;
    FaultEvidence evidence;
    auto operator<=>(const FaultFact&) const = default;
};

enum class FaultDisposition : std::uint32_t { COVERED, ESCALATED, UNRECOVERABLE };
enum class FallbackKind : std::uint32_t { PARENT_SERVICE_RESTART, BOARD_RESTART_REQUIRED };
struct FallbackAction {
    FallbackKind kind;
    std::set<FaultIdentity> triggering_faults;
    auto operator<=>(const FallbackAction&) const = default;
};
struct FaultCoverageEntry {
    FaultIdentity fault;
    FaultDisposition disposition;
    std::set<ResourceInstance> covering_resources;
    std::optional<FallbackAction> fallback;
    auto operator<=>(const FaultCoverageEntry&) const = default;
};
struct FaultCoverage {
    std::vector<FaultCoverageEntry> entries;
    auto operator<=>(const FaultCoverage&) const = default;
};
enum class DerivationOutcome : std::uint32_t {
    ONLINE_RECOVERY, FAIL_CLOSED, PARENT_SERVICE_RESTART, BOARD_RESTART_REQUIRED
};
struct RecoveryResourcePlan {
    ResourceInstance resource;
    ResourceKind resource_kind;
    RecoveryMode mode;
    EpochSnapshot old_epochs;
    std::set<EpochDomain> invalidated_domains;
    auto operator<=>(const RecoveryResourcePlan&) const = default;
};
struct RecoveryPlanDerivation {
    DerivationOutcome outcome;
    std::vector<RecoveryResourcePlan> resource_plans;
    FaultCoverage fault_coverage;
    std::optional<FallbackAction> fallback;
};
enum class ExecutorBindingStatus : std::uint32_t { RESOLVED, AMBIGUOUS, UNAUTHENTICATABLE };
struct OwnerExecutorBinding {
    ResourceInstance resource;
    ExecutorBindingStatus status;
    std::optional<ExecutorPrincipal> principal;
    std::uint64_t candidate_count;
    auto operator<=>(const OwnerExecutorBinding&) const = default;
};
struct DeviceTarget {
    DeviceContextIdentity device_context;
    auto operator<=>(const DeviceTarget&) const = default;
};
struct FrozenGrantRecord {
    ResourceInstance source;
    std::optional<ResourceInstance> destination;
    std::set<SubjectIdentity> execution_targets;
    std::set<DeviceTarget> device_targets;
    GrantIdentity identity{};
    OwnerId owner{};
    std::optional<LeaseIdentity> lease{};
    auto operator<=>(const FrozenGrantRecord&) const = default;
};
struct OwnerSnapshotAck {
    RecoveryInstanceId recovery_id;
    FreezeSeq freeze_seq;
    BootId boot_id;
    AdmissionGeneration closing_generation;
    OwnerId owner;
    std::set<GrantIdentity> locally_observed_grants;
    std::set<LeaseIdentity> outstanding_leases;
    Digest local_view_digest;
    auto operator<=>(const OwnerSnapshotAck&) const = default;
};
struct FrozenRuntimeSnapshot {
    RecoveryInstanceId recovery_id;
    FreezeSeq freeze_seq;
    BootId boot_id;
    AdmissionGeneration closing_generation{};
    GrantSeq grant_cutoff{};
    std::vector<FrozenGrantRecord> ledger_prefix{};
    Digest ledger_prefix_digest{};
    Digest owner_ack_canonical_digest{};
    std::vector<FaultFact> fault_batch;
    std::uint64_t fault_batch_cutoff{};
    Digest fault_batch_digest{};
    std::vector<OwnerExecutorBinding> owner_executor_bindings;
    std::vector<ProcessExitBindingDescriptor> process_exit_bindings;
    EpochSnapshot epoch_snapshot;
    Digest snapshot_digest;
};

enum class OperationKind : std::uint32_t {
    STREAMOFF, CLOSE_UNMAP_HANDLE, REQBUFS_ZERO, CLOSE_FD, RKNN_STOP, RGA_STOP,
    MPP_STOP, PACKET_STOP, RTMP_DISCONNECT, RECORD_STOP, INVALIDATE_EPOCH, REINIT,
    LOCAL_COMPLETE, COMMIT_EPOCH, REOPEN_ADMISSION
};
enum class ActionPhase : std::uint32_t {
    TEARDOWN, INVALIDATE_EPOCH, REINIT, LOCAL_COMPLETE, COMMIT_EPOCH, REOPEN_ADMISSION
};
struct GlobalScope { auto operator<=>(const GlobalScope&) const = default; };
using ActionScope = std::variant<ResourceInstance, GlobalScope>;
struct ActionNode {
    ActionId id;
    ActionPhase phase;
    OperationKind operation;
    ActionScope scope;
    EpochSnapshot old_epochs;
    std::optional<EpochSnapshot> new_epochs;
    std::set<ActionId> prerequisites;
    ExecutorPrincipal expected_executor;
    auto operator<=>(const ActionNode&) const = default;
};

struct CanonicalActionSpec {
    ActionPhase phase;
    OperationKind operation;
    std::set<std::size_t> prerequisite_indices;
    auto operator<=>(const CanonicalActionSpec&) const = default;
};
struct CanonicalActionRecipe {
    ResourceKind resource_kind{};
    RecoveryMode recovery_mode{};
    std::vector<CanonicalActionSpec> actions;
    auto operator<=>(const CanonicalActionRecipe&) const = default;
};
struct PolicyContents {
    std::map<std::pair<ResourceKind, RecoveryMode>, CanonicalActionRecipe> recipes;
    std::map<FaultKind, FaultDisposition> dispositions;
    std::map<FaultKind, FallbackKind> fallbacks;
    std::map<DeviceContextIdentity, CapabilityId> device_capability_map;
    std::map<ResourceKind, DeviceProofKind> resource_device_proof_kind;
    std::map<ResourceKind, EpochMask> resource_epoch_mask;
};
struct PolicySnapshot {
    PolicyVersion version;
    Digest digest;
    std::shared_ptr<const PolicyContents> contents;
};
struct EnvironmentFingerprint {
    Digest bsp_fingerprint;
    Digest kernel_build_id;
    Digest driver_version;
    Digest firmware_version;
    std::uint32_t api_mode;
    auto operator<=>(const EnvironmentFingerprint&) const = default;
};
struct CapabilityEvidence {
    CapabilityId capability_id;
    DeviceContextIdentity device_context;
    EnvironmentFingerprint environment;
    EvidenceId evidence_id;
    EvidenceDigest evidence_digest;
    TrustedMonotonicTime valid_from;
    TrustedMonotonicTime valid_until;
    auto operator<=>(const CapabilityEvidence&) const = default;
};
struct CapabilityRegistryContents {
    std::vector<CapabilityEvidence> evidences;
    std::set<EvidenceId> revoked_evidence;
    auto operator<=>(const CapabilityRegistryContents&) const = default;
};
struct CapabilityRegistrySnapshot {
    CapabilityRegistryVersion version;
    Digest digest;
    std::shared_ptr<const CapabilityRegistryContents> contents;
};

struct ExecutionPredicate {
    PredicateKey key;
    ExecutionProofKind required_kind;
    ProofIssuer required_issuer;
    SubjectIdentity expected_subject;
    EpochMask epoch_mask;
    std::optional<ProcessExitBindingId> expected_process_binding;
    std::optional<std::uint64_t> expected_spawn_generation;
    auto operator<=>(const ExecutionPredicate&) const = default;
};
struct CapabilityExpectation {
    CapabilityId capability_id;
    DeviceContextIdentity device_context;
    EnvironmentFingerprint environment;
    EvidenceId evidence_id;
    EvidenceDigest evidence_digest;
    TrustedMonotonicTime valid_from;
    TrustedMonotonicTime valid_until;
    auto operator<=>(const CapabilityExpectation&) const = default;
};
struct DevicePredicate {
    PredicateKey key;
    DeviceProofKind required_kind;
    ProofIssuer required_issuer;
    DeviceContextIdentity expected_device_context;
    DeviceTarget expected_target;
    EpochMask epoch_mask;
    CapabilityExpectation expected_capability;
    auto operator<=>(const DevicePredicate&) const = default;
};
struct DerivedPredicates {
    std::vector<ExecutionPredicate> execution;
    std::vector<DevicePredicate> device;
};
enum class PredicateDerivationFailureKind : std::uint32_t {
    REGISTRY_DIGEST_MISMATCH, RULE_MISSING, EVIDENCE_MISSING,
    EVIDENCE_AMBIGUOUS, EVIDENCE_REVOKED, EVIDENCE_EXPIRED,
    TARGET_NOT_IN_CLOSURE, EXECUTION_TARGET_UNRESOLVED
};
struct PredicateDerivationFailure {
    PredicateDerivationFailureKind kind;
    std::optional<DeviceContextIdentity> device_context;
    std::optional<SubjectIdentity> subject;
};
using PredicateDerivation = std::variant<DerivedPredicates, PredicateDerivationFailure>;

enum class ActionClosureFailureKind : std::uint32_t {
    RECIPE_MISSING, EXECUTOR_BINDING_MISSING, EXECUTOR_BINDING_AMBIGUOUS,
    EXECUTOR_BINDING_UNAUTHENTICATABLE, EPOCH_OVERFLOW
};
struct ActionClosureFailure {
    ActionClosureFailureKind kind;
    std::optional<ResourceInstance> resource;
    std::optional<EpochDomain> epoch_domain;
    auto operator<=>(const ActionClosureFailure&) const = default;
};
using ActionClosureResult = std::variant<std::vector<ActionNode>, ActionClosureFailure>;

struct CompileFailure {
    enum class Kind : std::uint32_t {
        EMPTY_FAULT_BATCH, FALLBACK_REQUIRED, EMPTY_PLAN, ACTION_CLOSURE_FAILED,
        INVALID_DAG, PREDICATE_DERIVATION_FAILED, TARGET_PREDICATE_MISMATCH,
        ALLOCATION_FAILED
    };
    Kind kind;
    std::optional<FallbackAction> fallback{};
    std::optional<PredicateDerivationFailure> predicate_failure{};
    std::optional<ActionClosureFailure> closure_failure{};
};

class RecoveryCompiler;
#ifdef RECOVERY_TESTING
class ValidatedRecoveryProgramTestAccess;
#endif
class ValidatedRecoveryProgram {
public:
    const RecoveryInstanceId& recovery_id() const noexcept { return recovery_id_; }
    FreezeSeq freeze_seq() const noexcept { return freeze_seq_; }
    BootId boot_id() const noexcept { return boot_id_; }
    const Digest& snapshot_digest() const noexcept { return snapshot_digest_; }
    const Digest& policy_digest() const noexcept { return policy_digest_; }
    CapabilityRegistryVersion registry_version() const noexcept { return registry_version_; }
    const Digest& registry_digest() const noexcept { return registry_digest_; }
    const std::vector<FaultFact>& faults() const noexcept { return faults_; }
    const std::vector<RecoveryResourcePlan>& resource_plans() const noexcept { return resource_plans_; }
    const FaultCoverage& fault_coverage() const noexcept { return fault_coverage_; }
    const std::vector<ExecutionPredicate>& execution_predicates() const noexcept { return execution_predicates_; }
    const std::vector<DevicePredicate>& device_predicates() const noexcept { return device_predicates_; }
    const std::vector<CapabilityExpectation>& capability_expectations() const noexcept { return capability_expectations_; }
    const std::vector<ActionNode>& action_dag() const noexcept { return action_dag_; }
    const TransactionHash& transaction_hash() const noexcept { return transaction_hash_; }
    ValidatedRecoveryProgram(const ValidatedRecoveryProgram&) = delete;
    ValidatedRecoveryProgram& operator=(const ValidatedRecoveryProgram&) = delete;
    ValidatedRecoveryProgram(ValidatedRecoveryProgram&&) noexcept = default;
    ValidatedRecoveryProgram& operator=(ValidatedRecoveryProgram&&) = delete;
private:
    friend class RecoveryCompiler;
#ifdef RECOVERY_TESTING
    friend class ValidatedRecoveryProgramTestAccess;
#endif
    RecoveryInstanceId recovery_id_;
    FreezeSeq freeze_seq_;
    BootId boot_id_;
    Digest snapshot_digest_;
    Digest policy_digest_;
    CapabilityRegistryVersion registry_version_;
    Digest registry_digest_;
    std::vector<FaultFact> faults_;
    std::vector<RecoveryResourcePlan> resource_plans_;
    FaultCoverage fault_coverage_;
    std::vector<ExecutionPredicate> execution_predicates_;
    std::vector<DevicePredicate> device_predicates_;
    std::vector<CapabilityExpectation> capability_expectations_;
    std::vector<ActionNode> action_dag_;
    TransactionHash transaction_hash_{};
    ValidatedRecoveryProgram(RecoveryInstanceId, FreezeSeq, BootId, Digest, Digest,
                             CapabilityRegistryVersion, Digest, std::vector<FaultFact>,
                             std::vector<RecoveryResourcePlan>, FaultCoverage,
                             std::vector<ExecutionPredicate>, std::vector<DevicePredicate>,
                             std::vector<CapabilityExpectation>, std::vector<ActionNode>);
};

using CompileResult = std::variant<ValidatedRecoveryProgram, CompileFailure>;

RecoveryPlanDerivation derive_resource_plans(
    const FrozenRuntimeSnapshot&, const std::vector<FaultFact>&,
    const PolicySnapshot&);
[[nodiscard]] Digest compute_transaction_hash(const ValidatedRecoveryProgram& program);

#define RECOVERY_CANONICAL_STRUCT(type, ...) \
    inline void canonical_write(CanonicalWriter& writer, const type& value) { __VA_ARGS__ }
inline void canonical_write(CanonicalWriter&, const ProofIssuer&);
RECOVERY_CANONICAL_STRUCT(EpochSnapshot,
    canonical_write(writer, value.stream_epoch); canonical_write(writer, value.ai_epoch);
    canonical_write(writer, value.worker_epoch); canonical_write(writer, value.media_epoch);)
RECOVERY_CANONICAL_STRUCT(FaultIdentity, canonical_write(writer, value.boot_id); canonical_write(writer, value.fault_seq);)
RECOVERY_CANONICAL_STRUCT(FaultSubject, canonical_write(writer, value.resource); canonical_write(writer, value.device_context); canonical_write(writer, value.process);)
RECOVERY_CANONICAL_STRUCT(FaultEvidence, canonical_write(writer, value.evidence_id); canonical_write(writer, value.evidence_digest);)
RECOVERY_CANONICAL_STRUCT(FaultFact, canonical_write(writer, value.id); canonical_write(writer, value.kind); canonical_write(writer, value.subject); canonical_write(writer, value.observed_epochs); canonical_write(writer, value.evidence);)
RECOVERY_CANONICAL_STRUCT(FallbackAction, canonical_write(writer, value.kind); canonical_write(writer, value.triggering_faults);)
RECOVERY_CANONICAL_STRUCT(FaultCoverageEntry, canonical_write(writer, value.fault); canonical_write(writer, value.disposition); canonical_write(writer, value.covering_resources); canonical_write(writer, value.fallback);)
RECOVERY_CANONICAL_STRUCT(FaultCoverage, canonical_write(writer, value.entries);)
RECOVERY_CANONICAL_STRUCT(RecoveryResourcePlan, canonical_write(writer, value.resource); canonical_write(writer, value.resource_kind); canonical_write(writer, value.mode); canonical_write(writer, value.old_epochs); canonical_write(writer, value.invalidated_domains);)
RECOVERY_CANONICAL_STRUCT(GlobalScope, (void)writer; (void)value;)
RECOVERY_CANONICAL_STRUCT(ActionNode, canonical_write(writer, value.id); canonical_write(writer, value.phase); canonical_write(writer, value.operation); canonical_write(writer, value.scope); canonical_write(writer, value.old_epochs); canonical_write(writer, value.new_epochs); canonical_write(writer, value.prerequisites); canonical_write(writer, value.expected_executor);)
RECOVERY_CANONICAL_STRUCT(CanonicalActionSpec, canonical_write(writer, value.phase); canonical_write(writer, value.operation); canonical_write(writer, value.prerequisite_indices);)
RECOVERY_CANONICAL_STRUCT(CanonicalActionRecipe, canonical_write(writer, value.resource_kind); canonical_write(writer, value.recovery_mode); canonical_write(writer, value.actions);)
RECOVERY_CANONICAL_STRUCT(PolicyContents, canonical_write(writer, value.recipes); canonical_write(writer, value.dispositions); canonical_write(writer, value.fallbacks); canonical_write(writer, value.device_capability_map); canonical_write(writer, value.resource_device_proof_kind); canonical_write(writer, value.resource_epoch_mask);)
RECOVERY_CANONICAL_STRUCT(EnvironmentFingerprint, canonical_write(writer, value.bsp_fingerprint); canonical_write(writer, value.kernel_build_id); canonical_write(writer, value.driver_version); canonical_write(writer, value.firmware_version); canonical_write(writer, value.api_mode);)
RECOVERY_CANONICAL_STRUCT(CapabilityEvidence, canonical_write(writer, value.capability_id); canonical_write(writer, value.device_context); canonical_write(writer, value.environment); canonical_write(writer, value.evidence_id); canonical_write(writer, value.evidence_digest); canonical_write(writer, value.valid_from); canonical_write(writer, value.valid_until);)
RECOVERY_CANONICAL_STRUCT(CapabilityRegistryContents, canonical_write(writer, value.evidences); canonical_write(writer, value.revoked_evidence);)
RECOVERY_CANONICAL_STRUCT(ExecutionPredicate, canonical_write(writer, value.key); canonical_write(writer, value.required_kind); canonical_write(writer, value.required_issuer); canonical_write(writer, value.expected_subject); canonical_write(writer, value.epoch_mask); canonical_write(writer, value.expected_process_binding); canonical_write(writer, value.expected_spawn_generation);)
RECOVERY_CANONICAL_STRUCT(CapabilityExpectation, canonical_write(writer, value.capability_id); canonical_write(writer, value.device_context); canonical_write(writer, value.environment); canonical_write(writer, value.evidence_id); canonical_write(writer, value.evidence_digest); canonical_write(writer, value.valid_from); canonical_write(writer, value.valid_until);)
RECOVERY_CANONICAL_STRUCT(DeviceTarget, canonical_write(writer, value.device_context);)
RECOVERY_CANONICAL_STRUCT(FrozenGrantRecord, canonical_write(writer, value.source); canonical_write(writer, value.destination); canonical_write(writer, value.execution_targets); canonical_write(writer, value.device_targets); canonical_write(writer, value.identity); canonical_write(writer, value.owner); canonical_write(writer, value.lease);)
RECOVERY_CANONICAL_STRUCT(OwnerSnapshotAck, canonical_write(writer, value.recovery_id); canonical_write(writer, value.freeze_seq); canonical_write(writer, value.boot_id); canonical_write(writer, value.closing_generation); canonical_write(writer, value.owner); canonical_write(writer, value.locally_observed_grants); canonical_write(writer, value.outstanding_leases); canonical_write(writer, value.local_view_digest);)
RECOVERY_CANONICAL_STRUCT(DevicePredicate, canonical_write(writer, value.key); canonical_write(writer, value.required_kind); canonical_write(writer, value.required_issuer); canonical_write(writer, value.expected_device_context); canonical_write(writer, value.expected_target); canonical_write(writer, value.epoch_mask); canonical_write(writer, value.expected_capability);)
#undef RECOVERY_CANONICAL_STRUCT

inline void canonical_write(CanonicalWriter& writer, const ThreadIdentity& value) {
    canonical_write(writer, value.pid); canonical_write(writer, value.tid); canonical_write(writer, value.start_generation);
}
inline void canonical_write(CanonicalWriter& writer, const ExecutionPredicateKey& value) {
    canonical_write(writer, value.kind); canonical_write(writer, value.subject);
}
inline void canonical_write(CanonicalWriter& writer, const DevicePredicateKey& value) {
    canonical_write(writer, value.kind); canonical_write(writer, value.device_context);
}
inline void canonical_write(CanonicalWriter& writer, const ProofIssuer& value) {
    canonical_write(writer, value.id); canonical_write(writer, value.role);
}
inline void canonical_write(CanonicalWriter& writer, const OwnerExecutorBinding& value) {
    canonical_write(writer, value.resource); canonical_write(writer, value.status);
    canonical_write(writer, value.principal); canonical_write(writer, value.candidate_count);
}

}  // namespace recovery

#endif
