#ifndef RECOVERY_INGRESS_HPP
#define RECOVERY_INGRESS_HPP

// Task 4I: TrustedFaultIngress session reader (§4.6, §14). Types follow the
// frozen design verbatim; only the slice exercised by G1 exists so far.

#include "recovery/canonical.hpp"
#include "recovery/runtime.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace recovery {

#ifdef RECOVERY_TESTING
struct RecoveryTestAccess;
#endif

// Three boot scopes, three strong types that never convert into one another
// or into an integer (G19).
struct LinuxBootId final {
    std::array<std::uint8_t, 16> bytes{};                  // full 128-bit UUID, never truncated
    auto operator<=>(const LinuxBootId&) const = default;
};
struct CoordinatorBootId final {
    std::uint64_t value{};
    auto operator<=>(const CoordinatorBootId&) const = default;
};
struct StoreDomainId final {
    Digest value{};
    auto operator<=>(const StoreDomainId&) const = default;
};
// Parses the 36-character 8-4-4-4-12 form of /proc/sys/kernel/random/boot_id
// (one trailing newline allowed). Anything else is rejected.
[[nodiscard]] std::optional<LinuxBootId> parse_linux_boot_id(std::string_view text) noexcept;
// Reads and parses /proc/sys/kernel/random/boot_id.
[[nodiscard]] std::optional<LinuxBootId> read_linux_boot_id() noexcept;
struct FaultDeliveryId final {
    Digest value{};
    auto operator<=>(const FaultDeliveryId&) const = default;
};
struct FaultSourceRegistrationId final {
    std::uint64_t value{};
    auto operator<=>(const FaultSourceRegistrationId&) const = default;
};
struct AuthenticatedSourceInstance final {
    LinuxBootId linux_boot;
    FaultSourceRegistrationId registration;
    ProcessIdentity process;
    auto operator<=>(const AuthenticatedSourceInstance&) const = default;
};
// Dedup scope: no connection, raw fd or pidfd value takes part (G2, G15).
struct DedupKey final {
    CoordinatorBootId coordinator_boot;
    AuthenticatedSourceInstance source;
    FaultDeliveryId delivery;
    auto operator<=>(const DedupKey&) const = default;
};
// The persisted form: LinuxBootId is exactly its 16 bytes, no length prefix.
void canonical_write(CanonicalWriter&, const LinuxBootId&);
void canonical_write(CanonicalWriter&, const DedupKey&);

struct FaultFrame final {
    std::uint32_t wire_version{};
    FaultDeliveryId delivery_id;
    FaultReport report;
    std::vector<std::uint8_t> authenticated_metadata;
};

// One SOCK_SEQPACKET packet carries exactly one frame. The design fixes the
// transport but not the byte layout; these two functions are the only place
// that layout lives.
[[nodiscard]] std::vector<std::uint8_t> encode_fault_frame(const FaultFrame&);
[[nodiscard]] std::optional<FaultFrame> decode_fault_frame(std::span<const std::uint8_t>);

enum class FaultSessionError : std::uint8_t {
    PEER_UNREGISTERED, REGISTRATION_REVOKED, PEER_CREDENTIAL_MISMATCH,
    SOURCE_UNAUTHORIZED, FRAME_INVALID, FRAME_TOO_LARGE, IO_FAILED, CAPABILITY_UNAVAILABLE
};
enum class FaultSourcePolicyDecision : std::uint8_t { ALLOW, DENY, REVOKED };
struct FaultSourcePolicyRule final {
    FaultKind kind;
    ResourceInstance resource;
    auto operator<=>(const FaultSourcePolicyRule&) const = default;
};
class FaultSourcePolicySnapshot final {
public:
    FaultSourcePolicySnapshot(const FaultSourcePolicySnapshot&) = delete;
    FaultSourcePolicySnapshot& operator=(const FaultSourcePolicySnapshot&) = delete;
    static std::variant<std::shared_ptr<const FaultSourcePolicySnapshot>, FaultSessionError>
        create(std::vector<FaultSourcePolicyRule>) noexcept;
    [[nodiscard]] FaultSourcePolicyDecision authorize(FaultKind, ResourceInstance) const noexcept;
    [[nodiscard]] Digest digest() const noexcept { return digest_; }
private:
    FaultSourcePolicySnapshot(std::vector<FaultSourcePolicyRule>&&, Digest) noexcept;
    std::vector<FaultSourcePolicyRule> rules_;
    Digest digest_{};
};
class FaultSourceRevocationState final {
public:
    [[nodiscard]] bool is_revoked_acquire() const noexcept;
    bool revoke_release() noexcept;
private:
    std::atomic<bool> revoked_{false};
};
struct FaultSourceRegistration final {
    FaultSourceRegistrationId id;
    AuthenticatedSourceInstance source;
    std::shared_ptr<const FaultSourcePolicySnapshot> policy;
    std::shared_ptr<const FaultSourceRevocationState> revocation;
};

enum class SessionTerminalCode : std::uint8_t {
    PEER_EOF, IO_ERROR, REJECTED_UNTRUSTED, INGRESS_NOT_RUNNING,
    TRANSPORT_CLOSED_AFTER_DELIVERED, BOOT_TERMINATING
};
struct SessionTerminalResult final {
    SessionTerminalCode code;
    bool operator==(const SessionTerminalResult&) const = default;
};

class FaultIngressSessionIo;
class AuthorizedFaultFrame final {
    friend class FaultIngressSessionIo;
    explicit AuthorizedFaultFrame(FaultFrame&& frame) noexcept : frame_(std::move(frame)) {}
    FaultFrame frame_;
public:
    AuthorizedFaultFrame() = delete;
    AuthorizedFaultFrame(const AuthorizedFaultFrame&) = delete;
    AuthorizedFaultFrame& operator=(const AuthorizedFaultFrame&) = delete;
    AuthorizedFaultFrame(AuthorizedFaultFrame&&) noexcept = default;
    AuthorizedFaultFrame& operator=(AuthorizedFaultFrame&&) = delete;
    [[nodiscard]] const FaultFrame& frame() const noexcept { return frame_; }
};
struct FrameContinue final {};
struct FrameAckSent final {};
struct FrameWaitTimedOut final {};
using SessionReadOutcome = std::variant<AuthorizedFaultFrame, SessionTerminalResult>;
using FrameOutcome = std::variant<FrameContinue, FrameAckSent, FrameWaitTimedOut,
                                  SessionTerminalResult>;
using FrameStepResult = FrameOutcome;

class FaultIngressAcceptor;
struct FaultPeerEof final {};  // recv returned 0: orderly peer close, never an error (G1)
class FaultIngressSession final {
public:
    FaultIngressSession(const FaultIngressSession&) = delete;
    FaultIngressSession& operator=(const FaultIngressSession&) = delete;
    FaultIngressSession(FaultIngressSession&&) noexcept = default;
    FaultIngressSession& operator=(FaultIngressSession&&) = delete;
    ~FaultIngressSession() noexcept = default;
    [[nodiscard]] const AuthenticatedSourceInstance& peer() const noexcept;
private:
    friend class FaultIngressAcceptor;
    friend class FaultIngressSessionIo;
#ifdef RECOVERY_TESTING
    friend struct RecoveryTestAccess;
#endif
    FaultIngressSession(OwnedFd connection, OwnedFd pidfd,
                        std::shared_ptr<const FaultSourceRegistration>) noexcept;
    std::variant<FaultFrame, FaultPeerEof, FaultSessionError> receive_frame();
    [[nodiscard]] FaultSourcePolicyDecision authorize_current(const FaultFrame&) const noexcept;
    OwnedFd connection_;
    OwnedFd pidfd_;
    std::shared_ptr<const FaultSourceRegistration> registration_;
};

class TrustedFaultIngress;
class FaultIngressSessionIo final {
    friend class TrustedFaultIngress;
#ifdef RECOVERY_TESTING
    friend struct RecoveryTestAccess;
#endif
    FaultIngressSessionIo() noexcept = default;
public:
    FaultIngressSessionIo(const FaultIngressSessionIo&) = delete;
    FaultIngressSessionIo& operator=(const FaultIngressSessionIo&) = delete;
    // receive + revocation acquire + policy authorize as one reader operation.
    [[nodiscard]] SessionReadOutcome read_authorized(FaultIngressSession&) const;
};

// ===== deadline ticket capability (§14). The scheduler itself is slice S5;
// the ticket exists now because every durable journal write is bound to one.
struct DeadlineTicketId final {
    std::uint64_t value{};
    auto operator<=>(const DeadlineTicketId&) const = default;
};
struct DeadlineSlotId final {
    std::uint32_t value;
    auto operator<=>(const DeadlineSlotId&) const = default;
};
enum class IngressDeadlineKind : std::uint8_t { PREPARED_JOURNAL_IO, OWNER_EXECUTION, DELIVERED_JOURNAL_IO };
class DeadlineScheduler;
class DeadlineTicket final {
    friend class DeadlineScheduler;
    friend class TrustedFaultIngress;
#ifdef RECOVERY_TESTING
    friend struct RecoveryTestAccess;
#endif
    DeadlineTicket(std::shared_ptr<DeadlineScheduler>, std::shared_ptr<TrustedFaultIngress>,
                   DeadlineTicketId, IngressDeadlineKind, DeadlineSlotId) noexcept;
    void disarm_after_cancelled() noexcept;
    std::shared_ptr<DeadlineScheduler> scheduler_;
    std::shared_ptr<TrustedFaultIngress> ingress_;
    DeadlineTicketId id_;
    IngressDeadlineKind kind_;
    DeadlineSlotId slot_;
    bool armed_{true};
public:
    DeadlineTicket() = delete;
    DeadlineTicket(const DeadlineTicket&) = delete;
    DeadlineTicket& operator=(const DeadlineTicket&) = delete;
    DeadlineTicket(DeadlineTicket&&) noexcept;
    DeadlineTicket& operator=(DeadlineTicket&&) = delete;
    ~DeadlineTicket() noexcept;   // armed ⇒ hard trip; never an implicit cancel
    [[nodiscard]] DeadlineTicketId id() const noexcept { return id_; }
    [[nodiscard]] IngressDeadlineKind kind() const noexcept { return kind_; }
    [[nodiscard]] DeadlineSlotId slot() const noexcept { return slot_; }
};

class TrustedFaultIngress final {
public:
    // The single handler entry: takes the session by value (§4.6.1). Its body
    // lands with the S3 ingress slice; nothing calls it before then.
    SessionTerminalResult handle_session(FaultIngressSession) noexcept;
    TrustedFaultIngress(const TrustedFaultIngress&) = delete;
    TrustedFaultIngress& operator=(const TrustedFaultIngress&) = delete;
private:
#ifdef RECOVERY_TESTING
    friend struct RecoveryTestAccess;
#endif
    friend class DeadlineTicket;
    TrustedFaultIngress() noexcept = default;
    // Contract violation: no unwinding, no lock, no allocation. Wired to the
    // HardWatchdogChannel with the lifecycle slice (S4).
    [[noreturn]] void hard_trip(IngressFailureCode) noexcept;
    FrameOutcome handle_frame(const AuthorizedFaultFrame&, FaultIngressSession&,
                              const FaultIngressSessionIo&) noexcept;
};

}  // namespace recovery
#endif
