#ifndef RECOVERY_TYPES_HPP
#define RECOVERY_TYPES_HPP

#include <array>
#include <compare>
#include <cstdint>
#include <utility>
#include <variant>

namespace recovery {

using Digest = std::array<std::uint8_t, 32>;
using TransactionHash = Digest;
using BootId = std::uint64_t;
using RecoverySeq = std::uint64_t;
using FreezeSeq = std::uint64_t;
using AdmissionGeneration = std::uint64_t;
using GrantSeq = std::uint64_t;
using LeaseIdentity = std::uint64_t;
using OwnerId = std::uint64_t;
using ProcessExitBindingSeq = std::uint64_t;
using ProcessId = std::uint64_t;
using ThreadId = std::uint64_t;
using ActionId = std::uint64_t;
using DeviceContextIdentity = std::uint64_t;
using IssuerId = std::uint64_t;

struct RecoveryInstanceId {
    BootId boot_id;
    RecoverySeq recovery_seq;
    std::uint64_t nonce;
    auto operator<=>(const RecoveryInstanceId&) const = default;
};
struct GrantIdentity {
    BootId boot_id;
    AdmissionGeneration admission_generation;
    GrantSeq seq;
    auto operator<=>(const GrantIdentity&) const = default;
};

struct ThreadIdentity {
    ProcessId pid;
    ThreadId tid;
    std::uint64_t start_generation;
    auto operator<=>(const ThreadIdentity&) const = default;
};

struct ProcessIdentity {
    ProcessId pid;
    std::uint64_t start_generation;
    auto operator<=>(const ProcessIdentity&) const = default;
};

struct ProcessExitBindingId {
    RecoveryInstanceId recovery_id;
    FreezeSeq freeze_seq;
    ProcessExitBindingSeq binding_seq;
    auto operator<=>(const ProcessExitBindingId&) const = default;
};

struct ProcessExitBindingDescriptor {
    ProcessExitBindingId id;
    ProcessIdentity process;
    auto operator<=>(const ProcessExitBindingDescriptor&) const = default;
};

using SubjectIdentity = std::variant<ThreadIdentity, ProcessIdentity>;

struct ProcessPrincipal {
    ProcessIdentity process;
    auto operator<=>(const ProcessPrincipal&) const = default;
};

struct CoordinatorPrincipal {
    auto operator<=>(const CoordinatorPrincipal&) const = default;
};

enum class IssuerRole : std::uint32_t {
    EXECUTION_AGENT,
    DEVICE_DRIVER,
    COORDINATOR,
};

struct ProofIssuer {
    IssuerId id;
    IssuerRole role;
    bool operator==(const ProofIssuer&) const = default;
};

using ExecutorPrincipal = std::variant<ProcessPrincipal, CoordinatorPrincipal>;
inline const ExecutorPrincipal kCoordinatorPrincipal = CoordinatorPrincipal{};

enum class ExecutionProofKind : std::uint32_t {
    THREADS_JOINED,
    PROCESS_EXIT_CONFIRMED,
};

enum class DeviceProofKind : std::uint32_t {
    DRIVER_FENCE_SIGNALED,
    CONTEXT_DESTROY_CONFIRMED,
    HARDWARE_RESET_CONFIRMED,
};

struct ExecutionPredicateKey {
    ExecutionProofKind kind;
    SubjectIdentity subject;
    auto operator<=>(const ExecutionPredicateKey&) const = default;
};

struct DevicePredicateKey {
    DeviceProofKind kind;
    DeviceContextIdentity device_context;
    auto operator<=>(const DevicePredicateKey&) const = default;
};

using PredicateKey = std::variant<ExecutionPredicateKey, DevicePredicateKey>;

#ifdef RECOVERY_TESTING
class PermitHandleTestAccess;
#endif

class PermitHandle {
    friend class AuthorizationJournal;
    friend class RecoveryCoordinator;
#ifdef RECOVERY_TESTING
    friend class PermitHandleTestAccess;
#endif

    RecoveryInstanceId recovery_id_;
    TransactionHash transaction_hash_;
    ActionId action_id_;
    std::uint64_t issue_nonce_;

    PermitHandle(RecoveryInstanceId recovery_id,
                 TransactionHash transaction_hash,
                 ActionId action_id,
                 std::uint64_t issue_nonce) noexcept
        : recovery_id_(recovery_id),
          transaction_hash_(transaction_hash),
          action_id_(action_id),
          issue_nonce_(issue_nonce) {}

public:
    PermitHandle() = delete;
    PermitHandle(const PermitHandle&) = delete;
    PermitHandle& operator=(const PermitHandle&) = delete;
    PermitHandle(PermitHandle&& other) noexcept
        : recovery_id_(std::move(other.recovery_id_)),
          transaction_hash_(std::move(other.transaction_hash_)),
          action_id_(std::exchange(other.action_id_, 0)),
          issue_nonce_(std::exchange(other.issue_nonce_, 0)) {}
    PermitHandle& operator=(PermitHandle&&) = delete;

    [[nodiscard]] bool valid() const noexcept { return issue_nonce_ != 0; }
};

}  // namespace recovery

#endif
