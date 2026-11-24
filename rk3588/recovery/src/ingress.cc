#include "recovery/ingress.hpp"

#include "recovery/canonical.hpp"
#include "recovery/canonical_reader.hpp"
#include "recovery/crypto.hpp"

#include <algorithm>
#include <concepts>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <type_traits>
#include <utility>

// Task 4I (G1): canonical fault-frame codec and the session reader that
// receives and authorizes in one step. handle_frame is still a placeholder.

namespace recovery {
namespace {
// Upper bound of one encoded frame; a larger packet is FRAME_TOO_LARGE.
constexpr std::size_t kMaxFaultFrameBytes = 64 * 1024;

void write_frame(CanonicalWriter& writer, const FaultFrame& frame) {
    canonical_write(writer, frame.wire_version);
    canonical_write(writer, frame.delivery_id.value);
    canonical_write(writer, frame.report.kind);
    canonical_write(writer, frame.report.subject);
    canonical_write(writer, frame.report.observed_epochs);
    canonical_write(writer, frame.report.evidence);
    canonical_write(writer, frame.authenticated_metadata);
}

template <typename Value, typename ReadInner>
bool read_optional(CanonicalReader& reader, std::optional<Value>& value, ReadInner inner) {
    bool present{};
    if (!reader.read_tag(present)) return false;
    if (!present) { value.reset(); return true; }
    Value read_value{};
    if (!inner(read_value)) return false;
    value = read_value;
    return true;
}

bool read_kind(CanonicalReader& reader, FaultKind& value) {
    std::underlying_type_t<FaultKind> raw{};
    if (!reader.read(raw) || raw > static_cast<decltype(raw)>(FaultKind::UNKNOWN)) return false;
    value = static_cast<FaultKind>(raw);
    return true;
}

bool read_process(CanonicalReader& reader, ProcessIdentity& value) {
    return reader.read(value.pid) && reader.read(value.start_generation);
}

bool read_subject(CanonicalReader& reader, FaultSubject& value) {
    return reader.read(value.resource)
        && read_optional(reader, value.device_context,
                         [&](DeviceContextIdentity& v) { return reader.read(v); })
        && read_optional(reader, value.process,
                         [&](ProcessIdentity& v) { return read_process(reader, v); });
}

bool read_epochs(CanonicalReader& reader, EpochSnapshot& value) {
    return reader.read(value.stream_epoch) && reader.read(value.ai_epoch)
        && reader.read(value.worker_epoch) && reader.read(value.media_epoch);
}

bool read_evidence(CanonicalReader& reader, FaultEvidence& value) {
    return reader.read(value.evidence_id) && reader.read(value.evidence_digest);
}
}  // namespace

std::optional<LinuxBootId> parse_linux_boot_id(std::string_view text) noexcept {
    if (!text.empty() && text.back() == '\n') text.remove_suffix(1);
    if (text.size() != 36) return std::nullopt;
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    LinuxBootId id;
    std::size_t out = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') return std::nullopt;
            ++i;
            continue;
        }
        const int high = nibble(text[i]);
        const int low = nibble(text[i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        id.bytes[out++] = static_cast<std::uint8_t>((high << 4) | low);
        i += 2;
    }
    return id;
}

std::optional<LinuxBootId> read_linux_boot_id() noexcept {
    const int fd = ::open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    const OwnedFd owned{fd};
    char buffer[64];
    const ssize_t n = ::read(owned.get(), buffer, sizeof buffer);
    if (n <= 0) return std::nullopt;
    return parse_linux_boot_id(std::string_view{buffer, static_cast<std::size_t>(n)});
}

void canonical_write(CanonicalWriter& writer, const LinuxBootId& id) {
    writer.write_bytes(id.bytes.data(), id.bytes.size());
}

void canonical_write(CanonicalWriter& writer, const DedupKey& key) {
    canonical_write(writer, key.coordinator_boot.value);
    canonical_write(writer, key.source.linux_boot);
    canonical_write(writer, key.source.registration.value);
    canonical_write(writer, key.source.process);
    canonical_write(writer, key.delivery.value);
}

std::vector<std::uint8_t> encode_fault_frame(const FaultFrame& frame) {
    CanonicalWriter writer;
    write_frame(writer, frame);
    return std::move(writer).finish();
}

std::optional<FaultFrame> decode_fault_frame(std::span<const std::uint8_t> bytes) {
    CanonicalReader reader{bytes};
    FaultFrame frame{};
    if (!reader.read(frame.wire_version) || !reader.read(frame.delivery_id.value)
        || !read_kind(reader, frame.report.kind) || !read_subject(reader, frame.report.subject)
        || !read_epochs(reader, frame.report.observed_epochs)
        || !read_evidence(reader, frame.report.evidence)
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
        std::sort(rules.begin(), rules.end());
        rules.erase(std::unique(rules.begin(), rules.end()), rules.end());
        std::vector<std::pair<FaultKind, ResourceInstance>> canonical;
        canonical.reserve(rules.size());
        for (const auto& rule : rules) canonical.emplace_back(rule.kind, rule.resource);
        const auto digest = canonical_digest(canonical);
        return std::shared_ptr<const FaultSourcePolicySnapshot>(
            new FaultSourcePolicySnapshot(std::move(rules), digest));
    } catch (...) {
        return FaultSessionError::CAPABILITY_UNAVAILABLE;
    }
}

FaultSourcePolicyDecision FaultSourcePolicySnapshot::authorize(
    FaultKind kind, ResourceInstance resource) const noexcept {
    return std::binary_search(rules_.begin(), rules_.end(), FaultSourcePolicyRule{kind, resource})
        ? FaultSourcePolicyDecision::ALLOW : FaultSourcePolicyDecision::DENY;
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

// recvmsg + MSG_TRUNC: one packet is one frame, an oversized packet is never
// truncated into a shorter "valid" one. A zero-length SEQPACKET packet reads
// the same as an orderly close; both end only the peer's own session.
std::variant<FaultFrame, FaultPeerEof, FaultSessionError> FaultIngressSession::receive_frame() {
    std::vector<std::uint8_t> buffer(kMaxFaultFrameBytes);
    iovec io{buffer.data(), buffer.size()};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    ssize_t received = -1;
    do {
        received = ::recvmsg(connection_.get(), &message, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);
    if (received < 0) return FaultSessionError::IO_FAILED;
    if (received == 0) return FaultPeerEof{};
    if ((message.msg_flags & MSG_TRUNC) != 0) return FaultSessionError::FRAME_TOO_LARGE;
    auto frame = decode_fault_frame(
        std::span<const std::uint8_t>{buffer.data(), static_cast<std::size_t>(received)});
    if (!frame) return FaultSessionError::FRAME_INVALID;
    return std::move(*frame);
}

FaultSourcePolicyDecision FaultIngressSession::authorize_current(
    const FaultFrame& frame) const noexcept {
    if (!registration_ || !registration_->policy || !registration_->revocation
        || registration_->revocation->is_revoked_acquire())
        return FaultSourcePolicyDecision::REVOKED;
    return registration_->policy->authorize(frame.report.kind, frame.report.subject.resource);
}

SessionReadOutcome FaultIngressSessionIo::read_authorized(FaultIngressSession& session) const {
    auto received = session.receive_frame();
    if (std::holds_alternative<FaultPeerEof>(received))
        return SessionTerminalResult{SessionTerminalCode::PEER_EOF};
    if (const auto* error = std::get_if<FaultSessionError>(&received)) {
        return SessionTerminalResult{*error == FaultSessionError::IO_FAILED
            ? SessionTerminalCode::IO_ERROR : SessionTerminalCode::REJECTED_UNTRUSTED};
    }
    auto& frame = std::get<FaultFrame>(received);
    if (session.authorize_current(frame) != FaultSourcePolicyDecision::ALLOW)
        return SessionTerminalResult{SessionTerminalCode::REJECTED_UNTRUSTED};
    return AuthorizedFaultFrame{std::move(frame)};
}

DeadlineTicket::DeadlineTicket(std::shared_ptr<DeadlineScheduler> scheduler,
                               std::shared_ptr<TrustedFaultIngress> ingress, DeadlineTicketId id,
                               IngressDeadlineKind kind, DeadlineSlotId slot) noexcept
    : scheduler_(std::move(scheduler)), ingress_(std::move(ingress)), id_(id), kind_(kind),
      slot_(slot) {}

DeadlineTicket::DeadlineTicket(DeadlineTicket&& other) noexcept
    : scheduler_(std::move(other.scheduler_)), ingress_(std::move(other.ingress_)),
      id_(other.id_), kind_(other.kind_), slot_(other.slot_),
      armed_(std::exchange(other.armed_, false)) {}

DeadlineTicket::~DeadlineTicket() noexcept {
    if (!armed_) return;
    if (ingress_) ingress_->hard_trip(IngressFailureCode::INTERNAL_FAILURE);
    fatal_watchdog_reboot("armed deadline ticket destroyed");
}

void DeadlineTicket::disarm_after_cancelled() noexcept { armed_ = false; }

void TrustedFaultIngress::hard_trip(IngressFailureCode) noexcept {
    fatal_watchdog_reboot("fault ingress hard trip");
}

FrameOutcome TrustedFaultIngress::handle_frame(const AuthorizedFaultFrame&, FaultIngressSession&,
                                               const FaultIngressSessionIo&) noexcept {
    return SessionTerminalResult{SessionTerminalCode::INGRESS_NOT_RUNNING};
}

}  // namespace recovery
