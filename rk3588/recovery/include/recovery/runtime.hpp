#ifndef RECOVERY_RUNTIME_HPP
#define RECOVERY_RUNTIME_HPP

#include "recovery/auth.hpp"
#include "recovery/compiler.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <variant>
#include <vector>
#include <string>

namespace recovery {

// Last-resort exit when the coordinator mutex cannot be acquired: no state is
// written and nothing unwinds. The board supervisor/watchdog turns this exit
// into a restart (§19, UNVERIFIED on target).
inline constexpr int kWatchdogRebootExitCode = 86;
[[noreturn]] void fatal_watchdog_reboot(const char* reason) noexcept;

#ifdef RECOVERY_TESTING
class RecoveryRuntimeTestAccess;
struct RecoveryTestAccess;
enum class RuntimeFailureStage : std::uint32_t {
    REPORT_PREPARE, REPORT_APPEND, BEGIN_FREEZE_PREPARE,
    BEGIN_FREEZE_AFTER_COPY, BEGIN_FREEZE_AFTER_DIGEST,
    FREEZE_TARGETS, FREEZE_OWNER_ACK, FREEZE_PREFIX_DIGEST,
    FREEZE_BATCH_PREPARE, FREEZE_BATCH_DIGEST, FREEZE_AFTER_BINDINGS,
    FREEZE_BINDING_LEGALITY, FREEZE_SNAPSHOT, FREEZE_SNAPSHOT_DIGEST,
    BEGIN_COMPILE_PUBLISH, FINISH_COMPILE_INSTALL, SEAL_PUBLISH,
    BEGIN_FREEZE_LOCK, FREEZE_LOCK, REPORT_LOCK, OWNER_ACK_LOCK, BEGIN_COMPILE_LOCK,
    FINISH_COMPILE_LOCK, FALLBACK_LOCK, SEAL_LOCK, SEAL_VERIFY, OWNER_ACK_INSERT
};
// A lock-acquire failure must end the process through fatal_watchdog_reboot,
// never through std::terminate (R166 ⑦, R167).
inline constexpr int kRecoveryTestWatchdogExitCode = kWatchdogRebootExitCode;
class RuntimeFailureProvider {
public:
    virtual ~RuntimeFailureProvider() = default;
    virtual void checkpoint(RuntimeFailureStage) = 0;
};
#endif

enum class RecoverySlotState : std::uint32_t {
    IDLE, PREPARING, FREEZING, FROZEN, COMPILING, SEALED, QUIESCING,
    EXECUTING, COMMITTING, REOPENING, FAIL_CLOSED
};

class OwnedFd {
public:
    OwnedFd() noexcept = default;
    explicit OwnedFd(int fd) noexcept : fd_(fd) {}
    ~OwnedFd() noexcept;
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    OwnedFd(OwnedFd&& other) noexcept;
    OwnedFd& operator=(OwnedFd&& other) noexcept;
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int get() const noexcept { return fd_; }
private:
    int fd_{-1};
};

struct OwnedProcessExitBinding {
    ProcessExitBindingDescriptor descriptor;
    OwnedFd pidfd;
};
struct FreezeRequest {
    RecoveryInstanceId recovery_id;
    FreezeSeq freeze_seq;
    BootId boot_id;
    AdmissionGeneration closing_generation;
    GrantSeq grant_cutoff;
    Digest ledger_prefix_digest;
    auto operator<=>(const FreezeRequest&) const = default;
};
enum class BeginFreezeErrorCode : std::uint8_t {
    WRONG_PHASE, SEQUENCE_EXHAUSTED, ALLOCATION_FAILED, OWNER_BINDING_INVALID, INTERNAL_FAILURE
};
struct BeginFreezeError final { BeginFreezeErrorCode code; };
using BeginFreezeResult = std::variant<FreezeRequest, BeginFreezeError>;
// ===== §6.2 owner set and ACK intake =====
// An owner that must ACK this freeze; no principal = coordinator-internal
// owner whose ACK begin_freeze synthesizes.
struct RequiredOwner {
    OwnerId owner;
    std::optional<ProcessPrincipal> principal;
    auto operator<=>(const RequiredOwner&) const = default;
};
struct OwnerBindingInvalid { OwnerId owner; };
using RequiredOwnersResult = std::variant<std::vector<RequiredOwner>, OwnerBindingInvalid>;
// Pure; reads only active records of the frozen prefix, OwnerId ascending. May allocate.
[[nodiscard]] RequiredOwnersResult derive_required_owners(
    const std::vector<FrozenGrantRecord>& ledger_prefix);
[[nodiscard]] std::set<GrantIdentity> expected_owner_grants(
    const std::vector<FrozenGrantRecord>& ledger_prefix, OwnerId owner);
[[nodiscard]] Digest recompute_owner_ack_digest(const OwnerSnapshotAck&);
[[nodiscard]] OwnerSnapshotAck make_internal_owner_ack(
    const FreezeRequest&, OwnerId, const std::vector<FrozenGrantRecord>& ledger_prefix);
[[nodiscard]] const RequiredOwner* find_required_owner(
    const std::vector<RequiredOwner>&, OwnerId) noexcept;
[[nodiscard]] bool all_required_owner_acks_present(
    const std::vector<RequiredOwner>&, const std::map<OwnerId, OwnerSnapshotAck>&) noexcept;
enum class OwnerAckErrorCode : std::uint8_t {
    WRONG_PHASE, UNAUTHORIZED, BINDING_MISMATCH, NOT_REQUIRED_OWNER,
    GRANT_VIEW_MISMATCH, DIGEST_MISMATCH, CONFLICT, ALLOCATION_FAILED
};
struct OwnerAckError final { OwnerAckErrorCode code; };
struct OwnerAckAccepted final { bool duplicate; };
using OwnerAckResult = std::variant<OwnerAckAccepted, OwnerAckError>;

using ProcessExitBindingRegistry =
    std::map<ProcessExitBindingId, OwnedProcessExitBinding>;

enum class ProcessExitBindFailure : std::uint32_t {
    READ_BEFORE_FAILED, GENERATION_MISMATCH_BEFORE, PIDFD_OPEN_FAILED,
    READ_AFTER_FAILED, GENERATION_MISMATCH_AFTER
};

class ProcessExitKernel {
public:
    virtual ~ProcessExitKernel() = default;
    virtual std::optional<std::uint64_t> read_start_generation(ProcessId) = 0;
    virtual OwnedFd pidfd_open(ProcessId) = 0;
};

struct RecoveryInstanceRuntime {
    RecoveryInstanceId recovery_id;
    std::vector<FaultFact> fault_batch;
    std::vector<FrozenGrantRecord> ledger_prefix;
    GrantSeq grant_cutoff{};
    Digest ledger_prefix_digest{};
    std::vector<OwnerExecutorBinding> owner_executor_bindings;
    std::vector<RequiredOwner> required_owners;               // set once by begin_freeze
    std::map<OwnerId, OwnerSnapshotAck> accepted_owner_acks;  // internal ACKs + §6.2 intake
    std::optional<FrozenRuntimeSnapshot> current_snapshot;
    ProcessExitBindingRegistry process_exit_binding_registry;
    PolicySnapshot current_policy{};
    CapabilityRegistrySnapshot current_registry{};
    std::shared_ptr<const ValidatedRecoveryProgram> compiled_program;
    std::shared_ptr<const ValidatedRecoveryProgram> current_program;
    TransactionHash transaction_hash{};
    bool fallback_pending{};
    std::optional<FallbackAction> pending_fallback;

    RecoveryInstanceRuntime(RecoveryInstanceId id, FaultFact first);
    RecoveryInstanceRuntime(RecoveryInstanceRuntime&&) noexcept = default;
    RecoveryInstanceRuntime& operator=(RecoveryInstanceRuntime&&) noexcept = default;
    RecoveryInstanceRuntime(const RecoveryInstanceRuntime&) = delete;
    RecoveryInstanceRuntime& operator=(const RecoveryInstanceRuntime&) = delete;
};

struct RecoverySlot {
    BootId boot_id{};
    RecoverySeq recovery_seq{};
    FreezeSeq freeze_seq{};
    RecoverySlotState state{RecoverySlotState::IDLE};
    EpochSnapshot epochs{};
    AdmissionGeneration closing_generation{};
    std::uint64_t fault_seq_counter{};
    std::optional<RecoveryInstanceRuntime> active;
    std::string fail_reason;
};

[[nodiscard]] FallbackKind worst_fallback(FallbackKind, FallbackKind) noexcept;
[[nodiscard]] bool descriptors_registry_agree(
    const std::vector<ProcessExitBindingDescriptor>&,
    const ProcessExitBindingRegistry&) noexcept;

class RecoveryCoordinator {
public:
    explicit RecoveryCoordinator(BootId, ProcessExitKernel* = nullptr);
#ifdef RECOVERY_TESTING
    RecoveryCoordinator(BootId, ProcessExitKernel*, RuntimeFailureProvider*);
#endif
    [[nodiscard]] bool report_fault(const FaultReport&) noexcept;
    [[nodiscard]] BeginFreezeResult begin_freeze(
        const std::vector<FrozenGrantRecord>&,
        const std::vector<OwnerExecutorBinding>& = {}) noexcept;
    [[nodiscard]] bool commit_freeze() noexcept;
    [[nodiscard]] OwnerAckResult submit_owner_snapshot_ack(
        const AuthenticatedExecutorContext&, const OwnerSnapshotAck&) noexcept;
    [[nodiscard]] bool begin_compile(PolicySnapshot = {},
                                     CapabilityRegistrySnapshot = {}) noexcept;
    void finish_compile(CompileResult) noexcept;
    void execute_fallback(std::optional<FallbackAction>) noexcept;
    [[nodiscard]] bool seal_validated_program(
        const std::shared_ptr<const ValidatedRecoveryProgram>&) noexcept;
    [[nodiscard]] RecoverySlotState state() const noexcept { return slot_.state; }
    [[nodiscard]] bool has_active() const noexcept { return slot_.active.has_value(); }
    [[nodiscard]] const RecoveryInstanceRuntime& active() const { return *slot_.active; }
private:
#ifdef RECOVERY_TESTING
    friend class RecoveryRuntimeTestAccess;
    friend struct RecoveryTestAccess;
#endif
    mutable std::mutex mutex_;
    RecoverySlot slot_;
    ProcessExitKernel* kernel_;
#ifdef RECOVERY_TESTING
    RuntimeFailureProvider* failure_provider_{};
    void checkpoint(RuntimeFailureStage);
#endif
    struct PreparedFaultBatchClose {
        std::vector<FaultFact> fault_batch;
        std::uint64_t fault_batch_cutoff{};
        Digest fault_batch_digest{};
    };
    [[nodiscard]] std::optional<PreparedFaultBatchClose>
    prepare_fault_batch_close() const;
    void fail_closed_locked(const char*) noexcept;
    // §4.4 lock template: acquire inside a try whose only landing point is
    // fatal_watchdog_reboot. No state is written before the lock is held.
#ifdef RECOVERY_TESTING
    [[nodiscard]] std::unique_lock<std::mutex> lock_or_watchdog(
        RuntimeFailureStage, const char* reason) noexcept;
#else
    [[nodiscard]] std::unique_lock<std::mutex> lock_or_watchdog(const char* reason) noexcept;
#endif
};

}  // namespace recovery
#endif
