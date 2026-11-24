#include "recovery/ingress.hpp"

// Task 4I RED skeleton (G1). Every function below is a placeholder that
// compiles and fails G1 on purpose; none of it is the §4.6 reader yet.

namespace recovery {

std::vector<std::uint8_t> encode_fault_frame(const FaultFrame&) { return {}; }

std::optional<FaultFrame> decode_fault_frame(std::span<const std::uint8_t>) {
    return std::nullopt;
}

FaultSourcePolicySnapshot::FaultSourcePolicySnapshot(
    std::vector<FaultSourcePolicyRule>&& rules, Digest digest) noexcept
    : rules_(std::move(rules)), digest_(digest) {}

std::variant<std::shared_ptr<const FaultSourcePolicySnapshot>, FaultSessionError>
FaultSourcePolicySnapshot::create(std::vector<FaultSourcePolicyRule> rules) noexcept {
    try {
        return std::shared_ptr<const FaultSourcePolicySnapshot>(
            new FaultSourcePolicySnapshot(std::move(rules), Digest{}));
    } catch (...) {
        return FaultSessionError::CAPABILITY_UNAVAILABLE;
    }
}

FaultSourcePolicyDecision FaultSourcePolicySnapshot::authorize(
    FaultKind, ResourceInstance) const noexcept {
    return FaultSourcePolicyDecision::DENY;
}

bool FaultSourceRevocationState::is_revoked_acquire() const noexcept {
    return revoked_.load(std::memory_order_acquire);
}

bool FaultSourceRevocationState::revoke_release() noexcept {
    return !revoked_.exchange(true, std::memory_order_release);
}

FaultIngressSession::FaultIngressSession(
    OwnedFd connection, OwnedFd pidfd,
    std::shared_ptr<const FaultSourceRegistration> registration) noexcept
    : connection_(std::move(connection)), pidfd_(std::move(pidfd)),
      registration_(std::move(registration)) {}

const AuthenticatedSourceInstance& FaultIngressSession::peer() const noexcept {
    return registration_->source;
}

std::variant<FaultFrame, FaultPeerEof, FaultSessionError> FaultIngressSession::receive_frame() {
    return FaultSessionError::IO_FAILED;
}

FaultSourcePolicyDecision FaultIngressSession::authorize_current(const FaultFrame&) const noexcept {
    return FaultSourcePolicyDecision::DENY;
}

SessionReadOutcome FaultIngressSessionIo::read_authorized(FaultIngressSession&) const {
    return SessionTerminalResult{SessionTerminalCode::IO_ERROR};
}

FrameOutcome TrustedFaultIngress::handle_frame(const AuthorizedFaultFrame&, FaultIngressSession&,
                                               const FaultIngressSessionIo&) noexcept {
    return SessionTerminalResult{SessionTerminalCode::INGRESS_NOT_RUNNING};
}

}  // namespace recovery
