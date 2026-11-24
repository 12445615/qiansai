// G1: the session reader receives and authorizes in one step; EOF and I/O
// error are distinct terminals (§4.6 items 1–2, §18A G1).
#include "recovery/ingress.hpp"
#include "test_support.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace recovery {

FaultIngressSession RecoveryTestAccess::make_fault_session(
    OwnedFd connection, std::shared_ptr<const FaultSourceRegistration> registration) {
    return FaultIngressSession{std::move(connection), OwnedFd{}, std::move(registration)};
}

const FaultIngressSessionIo& RecoveryTestAccess::session_io() {
    static const FaultIngressSessionIo io;
    return io;
}

}  // namespace recovery

namespace {
using recovery::SessionTerminalCode;
using Access = recovery::RecoveryTestAccess;

constexpr recovery::ResourceInstance kAllowedResource = 1;
constexpr recovery::ResourceInstance kDeniedResource = 2;

std::shared_ptr<const recovery::FaultSourceRegistration> registration(
    std::shared_ptr<const recovery::FaultSourceRevocationState> revocation =
        std::make_shared<const recovery::FaultSourceRevocationState>()) {
    auto policy = recovery::FaultSourcePolicySnapshot::create(
        {{recovery::FaultKind::PACKET_DOMAIN_FAULT, kAllowedResource}});
    RECOVERY_CHECK(std::holds_alternative<
        std::shared_ptr<const recovery::FaultSourcePolicySnapshot>>(policy));
    return std::make_shared<const recovery::FaultSourceRegistration>(
        recovery::FaultSourceRegistration{
            {7}, {{}, {7}, {4242, 99}},
            std::get<std::shared_ptr<const recovery::FaultSourcePolicySnapshot>>(policy),
            std::move(revocation)});
}

recovery::FaultFrame frame(recovery::ResourceInstance resource, std::uint8_t delivery) {
    recovery::FaultFrame value{};
    value.wire_version = 1;
    value.delivery_id.value[0] = delivery;
    value.report = {recovery::FaultKind::PACKET_DOMAIN_FAULT,
                    {resource, std::nullopt, std::nullopt}, {1, 2, 3, 4}, {delivery, {}}};
    return value;
}

struct SessionPair {
    recovery::OwnedFd peer;
    recovery::OwnedFd local;
};

SessionPair seqpacket_pair() {
    int fds[2] = {-1, -1};
    RECOVERY_CHECK_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds), 0);
    return {recovery::OwnedFd{fds[0]}, recovery::OwnedFd{fds[1]}};
}

void send_frame(const recovery::OwnedFd& peer, const recovery::FaultFrame& value) {
    const auto bytes = recovery::encode_fault_frame(value);
    RECOVERY_CHECK(!bytes.empty());
    RECOVERY_CHECK_EQ(::send(peer.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL),
                      static_cast<ssize_t>(bytes.size()));
}

std::optional<SessionTerminalCode> terminal_of(const recovery::SessionReadOutcome& outcome) {
    if (const auto* terminal = std::get_if<recovery::SessionTerminalResult>(&outcome))
        return terminal->code;
    return std::nullopt;
}
}  // namespace

RECOVERY_TEST(G1_frame_codec_round_trips_one_frame_per_packet) {
    const auto sent = frame(kAllowedResource, 1);
    const auto bytes = recovery::encode_fault_frame(sent);
    RECOVERY_CHECK(!bytes.empty());
    const auto decoded = recovery::decode_fault_frame(bytes);
    RECOVERY_CHECK(decoded.has_value());
    RECOVERY_CHECK(recovery::encode_fault_frame(*decoded) == bytes);
}

RECOVERY_TEST(G1_frame_codec_rejects_non_canonical_bytes) {
    auto sent = frame(kAllowedResource, 1);
    sent.authenticated_metadata = {9, 8, 7};
    const auto bytes = recovery::encode_fault_frame(sent);
    RECOVERY_CHECK(recovery::decode_fault_frame(bytes).has_value());

    auto truncated = bytes;
    truncated.pop_back();
    RECOVERY_CHECK(!recovery::decode_fault_frame(truncated).has_value());

    auto trailing = bytes;
    trailing.push_back(0);
    RECOVERY_CHECK(!recovery::decode_fault_frame(trailing).has_value());

    // wire_version(4) + delivery count(4) + delivery(32) precede the kind.
    constexpr std::size_t kKindOffset = 4 + 4 + 32;
    auto bad_kind = bytes;
    bad_kind[kKindOffset] = 0xff;
    RECOVERY_CHECK(!recovery::decode_fault_frame(bad_kind).has_value());

    // kind(4) + resource(8) precede the device_context optional tag.
    auto bad_tag = bytes;
    bad_tag[kKindOffset + 4 + 8] = 2;
    RECOVERY_CHECK(!recovery::decode_fault_frame(bad_tag).has_value());

    auto long_metadata = bytes;
    long_metadata[long_metadata.size() - 3 - 4] = 0xff;  // metadata count claims 255 bytes
    RECOVERY_CHECK(!recovery::decode_fault_frame(long_metadata).has_value());
}

RECOVERY_TEST(G1_reader_authorizes_inside_receive_and_rejection_is_terminal) {
    auto pair = seqpacket_pair();
    auto session = Access::make_fault_session(std::move(pair.local), registration());
    const auto& io = Access::session_io();
    const auto allowed = frame(kAllowedResource, 1);
    send_frame(pair.peer, allowed);
    send_frame(pair.peer, frame(kDeniedResource, 2));

    const auto first = io.read_authorized(session);
    const auto* authorized = std::get_if<recovery::AuthorizedFaultFrame>(&first);
    RECOVERY_CHECK(authorized != nullptr);
    RECOVERY_CHECK(recovery::encode_fault_frame(authorized->frame())
                   == recovery::encode_fault_frame(allowed));

    const auto second = io.read_authorized(session);
    RECOVERY_CHECK(terminal_of(second) == SessionTerminalCode::REJECTED_UNTRUSTED);
}

RECOVERY_TEST(G1_untrusted_bytes_and_revocation_end_the_session_as_rejected) {
    const auto& io = Access::session_io();
    {
        auto pair = seqpacket_pair();
        auto session = Access::make_fault_session(std::move(pair.local), registration());
        const std::uint8_t garbage[] = {1, 2, 3};
        RECOVERY_CHECK_EQ(::send(pair.peer.get(), garbage, sizeof garbage, MSG_NOSIGNAL),
                          static_cast<ssize_t>(sizeof garbage));
        RECOVERY_CHECK(terminal_of(io.read_authorized(session))
                       == SessionTerminalCode::REJECTED_UNTRUSTED);
    }
    {
        // Oversized packets are rejected whole (MSG_TRUNC), never cut into a frame.
        auto pair = seqpacket_pair();
        const int size = 128 * 1024;
        RECOVERY_CHECK_EQ(::setsockopt(pair.peer.get(), SOL_SOCKET, SO_SNDBUF, &size, sizeof size), 0);
        RECOVERY_CHECK_EQ(::setsockopt(pair.local.get(), SOL_SOCKET, SO_RCVBUF, &size, sizeof size), 0);
        auto session = Access::make_fault_session(std::move(pair.local), registration());
        auto oversized = frame(kAllowedResource, 1);
        oversized.authenticated_metadata.assign(70 * 1024, 0x5a);
        send_frame(pair.peer, oversized);
        RECOVERY_CHECK(terminal_of(io.read_authorized(session))
                       == SessionTerminalCode::REJECTED_UNTRUSTED);
    }
    {
        // Revocation is acquired per frame, so an allowed frame is still refused.
        auto revocation = std::make_shared<recovery::FaultSourceRevocationState>();
        auto pair = seqpacket_pair();
        auto session = Access::make_fault_session(std::move(pair.local), registration(revocation));
        RECOVERY_CHECK(revocation->revoke_release());
        send_frame(pair.peer, frame(kAllowedResource, 1));
        RECOVERY_CHECK(terminal_of(io.read_authorized(session))
                       == SessionTerminalCode::REJECTED_UNTRUSTED);
    }
}

RECOVERY_TEST(G1_peer_eof_is_distinct_from_io_error) {
    {
        auto pair = seqpacket_pair();
        auto session = Access::make_fault_session(std::move(pair.local), registration());
        pair.peer = recovery::OwnedFd{};  // orderly close: zero-length read
        const auto outcome = Access::session_io().read_authorized(session);
        RECOVERY_CHECK(terminal_of(outcome) == SessionTerminalCode::PEER_EOF);
    }
    {
        // recvmsg on a non-socket fails with ENOTSOCK: a real I/O error, not EOF.
        int pipe_fds[2] = {-1, -1};
        RECOVERY_CHECK_EQ(::pipe2(pipe_fds, O_CLOEXEC), 0);
        recovery::OwnedFd write_end{pipe_fds[1]};
        auto session = Access::make_fault_session(recovery::OwnedFd{pipe_fds[0]},
                                                  registration());
        const auto outcome = Access::session_io().read_authorized(session);
        RECOVERY_CHECK(terminal_of(outcome) == SessionTerminalCode::IO_ERROR);
    }
}

RECOVERY_TEST(G1_frame_outcome_carries_session_terminal) {
    const recovery::FrameOutcome outcome =
        recovery::SessionTerminalResult{SessionTerminalCode::PEER_EOF};
    RECOVERY_CHECK(std::holds_alternative<recovery::SessionTerminalResult>(outcome));
    static_assert(!std::is_default_constructible_v<recovery::AuthorizedFaultFrame>);
    static_assert(!std::is_copy_constructible_v<recovery::AuthorizedFaultFrame>);
    static_assert(!std::is_constructible_v<recovery::AuthorizedFaultFrame,
                                           recovery::FaultFrame&&>);
}
