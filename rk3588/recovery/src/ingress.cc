#include "recovery/ingress.hpp"

#include "recovery/canonical.hpp"

#include <algorithm>
#include <concepts>
#include <type_traits>

// Task 4I (G1). The frame codec is the repository's canonical encoding; the
// session reader below is still a placeholder that fails G1 on purpose.

namespace recovery {
namespace {
void write_frame(CanonicalWriter& writer, const FaultFrame& frame) {
    canonical_write(writer, frame.wire_version);
    canonical_write(writer, frame.delivery_id.value);
    canonical_write(writer, frame.report.kind);
    canonical_write(writer, frame.report.subject);
    canonical_write(writer, frame.report.observed_epochs);
    canonical_write(writer, frame.report.evidence);
    canonical_write(writer, frame.authenticated_metadata);
}

// Mirror of CanonicalWriter for the fields a frame carries. Any short read
// or malformed tag fails; canonicality is enforced by re-encoding.
class CanonicalReader {
public:
    explicit CanonicalReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    [[nodiscard]] bool exhausted() const { return offset_ == bytes_.size(); }

    template <std::unsigned_integral Integer>
    bool read(Integer& value) {
        if (bytes_.size() - offset_ < sizeof(Integer)) return false;
        value = 0;
        for (std::size_t index = 0; index < sizeof(Integer); ++index)
            value |= static_cast<Integer>(Integer{bytes_[offset_ + index]} << (8U * index));
        offset_ += sizeof(Integer);
        return true;
    }
    bool read_count(std::size_t& count) {
        std::uint32_t value{};
        if (!read(value)) return false;
        count = value;
        return true;
    }
    template <std::size_t Size>
    bool read(std::array<std::uint8_t, Size>& value) {
        std::size_t count{};
        if (!read_count(count) || count != Size) return false;
        for (auto& byte : value)
            if (!read(byte)) return false;
        return true;
    }
    bool read(std::vector<std::uint8_t>& value) {
        std::size_t count{};
        if (!read_count(count) || count > bytes_.size() - offset_) return false;
        value.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                     bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + count));
        offset_ += count;
        return true;
    }
    template <typename Value>
    bool read(std::optional<Value>& value) {
        std::uint8_t tag{};
        if (!read(tag) || tag > 1) return false;
        if (tag == 0) { value.reset(); return true; }
        Value inner{};
        if (!read(inner)) return false;
        value = inner;
        return true;
    }
    bool read(FaultKind& value) {
        std::underlying_type_t<FaultKind> raw{};
        if (!read(raw) || raw > static_cast<decltype(raw)>(FaultKind::UNKNOWN)) return false;
        value = static_cast<FaultKind>(raw);
        return true;
    }
    bool read(ProcessIdentity& value) {
        return read(value.pid) && read(value.start_generation);
    }
    bool read(FaultSubject& value) {
        return read(value.resource) && read(value.device_context) && read(value.process);
    }
    bool read(EpochSnapshot& value) {
        return read(value.stream_epoch) && read(value.ai_epoch)
            && read(value.worker_epoch) && read(value.media_epoch);
    }
    bool read(FaultEvidence& value) {
        return read(value.evidence_id) && read(value.evidence_digest);
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{};
};
}  // namespace

std::vector<std::uint8_t> encode_fault_frame(const FaultFrame& frame) {
    CanonicalWriter writer;
    write_frame(writer, frame);
    return std::move(writer).finish();
}

std::optional<FaultFrame> decode_fault_frame(std::span<const std::uint8_t> bytes) {
    CanonicalReader reader{bytes};
    FaultFrame frame{};
    if (!reader.read(frame.wire_version) || !reader.read(frame.delivery_id.value)
        || !reader.read(frame.report.kind) || !reader.read(frame.report.subject)
        || !reader.read(frame.report.observed_epochs) || !reader.read(frame.report.evidence)
        || !reader.read(frame.authenticated_metadata) || !reader.exhausted())
        return std::nullopt;
    // One frame has exactly one byte form.
    const auto reencoded = encode_fault_frame(frame);
    if (!std::equal(reencoded.begin(), reencoded.end(), bytes.begin(), bytes.end()))
        return std::nullopt;
    return frame;
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
