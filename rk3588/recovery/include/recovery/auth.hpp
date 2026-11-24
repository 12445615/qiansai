#ifndef RECOVERY_AUTH_HPP
#define RECOVERY_AUTH_HPP

// Executor authentication core (§5): the unforgeable context capability and
// the trusted connection table it is checked against. The socket side that
// registers connections and issues contexts (TrustedIpcAcceptor, R119–R161)
// does not exist yet, so a production build issues no context at all.

#include "recovery/types.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>

namespace recovery {

#ifdef RECOVERY_TESTING
struct RecoveryTestAccess;
#endif

using SpawnGeneration = std::uint64_t;

struct ChannelId {                       // 0 is invalid; never a bare uint64
    std::uint64_t value;
    auto operator<=>(const ChannelId&) const = default;
    [[nodiscard]] bool valid() const noexcept { return value != 0; }
};
enum class AuthErrorCode {
    UNAUTHENTICATED_CONTEXT, CHANNEL_ID_EXHAUSTED, CHANNEL_DUPLICATE, CONNECTION_NOT_OWNED,
    PEER_CREDENTIAL_UNAVAILABLE, PIDFD_UNAVAILABLE, START_GENERATION_UNAVAILABLE,
    CSPRNG_UNAVAILABLE, ALLOCATION_FAILED, UNKNOWN_CHANNEL, ACCEPT_FAILED, ACCEPTOR_STOPPED,
    LISTEN_FD_INVALID, INVALID_SESSION, SESSIONS_STILL_OPEN, SHUTDOWN_FAILED,
    IPC_LIFECYCLE_INVALID, IO_FAILED, PROTOCOL_ERROR, INTERNAL_FAILURE, ADMISSION_CLOSED,
    GRANT_SEQ_EXHAUSTED, GRANT_NOT_FOUND, GRANT_STATE_MISMATCH, NOT_ENDPOINT_OWNER
};
struct AuthError { AuthErrorCode code; };
struct AuthOk {};
using AuthStatus = std::variant<AuthOk, AuthError>;
inline AuthStatus auth_ok() noexcept { return AuthStatus{AuthOk{}}; }
inline AuthStatus auth_err(AuthErrorCode c) noexcept { return AuthStatus{AuthError{c}}; }

class AuthenticatedChannel {
    friend class ExecutorAuthenticator;
    ChannelId channel_id_;
    SpawnGeneration peer_start_generation_;
    explicit AuthenticatedChannel(ChannelId id, SpawnGeneration g) noexcept
        : channel_id_(id), peer_start_generation_(g) {}
public:
    AuthenticatedChannel() = delete;
    auto operator<=>(const AuthenticatedChannel&) const = default;
};

class AuthenticatedExecutorContext {
public:
    [[nodiscard]] const ExecutorPrincipal& principal() const noexcept { return principal_; }
    [[nodiscard]] std::optional<ProcessPrincipal> process() const noexcept {
        if (const auto* p = std::get_if<ProcessPrincipal>(&principal_)) return *p;
        return std::nullopt;
    }
    AuthenticatedExecutorContext(const AuthenticatedExecutorContext&) = delete;
    AuthenticatedExecutorContext& operator=(const AuthenticatedExecutorContext&) = delete;
    AuthenticatedExecutorContext(AuthenticatedExecutorContext&&) noexcept = default;
    AuthenticatedExecutorContext& operator=(AuthenticatedExecutorContext&&) noexcept = default;
private:
    friend class ExecutorAuthenticator;
    ExecutorPrincipal principal_;
    AuthenticatedChannel channel_;
    std::uint64_t channel_nonce_;
    Digest credential_digest_;
    AuthenticatedExecutorContext(ExecutorPrincipal p, AuthenticatedChannel ch,
                                 std::uint64_t nonce, Digest cred) noexcept
        : principal_(std::move(p)), channel_(ch), channel_nonce_(nonce),
          credential_digest_(cred) {}
};

class ExecutorAuthenticator {
public:
    // Read-only check of a context against the trusted connection table:
    // stale, forged, cross-channel or coordinator contexts are false.
    static bool verify_context(const AuthenticatedExecutorContext& ctx) noexcept {
        try {
            if (std::holds_alternative<CoordinatorPrincipal>(ctx.principal_)) return false;
            std::scoped_lock guard{channel_mutex_};
            const auto it = channels_.find(ctx.channel_.channel_id_);
            if (it == channels_.end()) return false;
            if (it->second.peer_start_generation != ctx.channel_.peer_start_generation_) return false;
            if (it->second.peer_credential_digest != ctx.credential_digest_) return false;
            if (it->second.session_nonce != ctx.channel_nonce_) return false;
            return true;
        } catch (...) { return false; }
    }
private:
    friend class TrustedIpcAcceptor;
#ifdef RECOVERY_TESTING
    friend struct RecoveryTestAccess;
#endif
    struct ChannelEntry {
        SpawnGeneration peer_start_generation;
        Digest peer_credential_digest;
        std::uint64_t session_nonce;
    };
    // OS facts only: built by the trusted acceptor, never from a payload.
    struct PeerAuthFacts {
        ProcessId pid;
        SpawnGeneration start_generation;
        Digest credential_digest;
        std::uint64_t session_nonce;
    private:
        friend class TrustedIpcAcceptor;
#ifdef RECOVERY_TESTING
        friend struct RecoveryTestAccess;
#endif
        PeerAuthFacts(ProcessId p, SpawnGeneration g, Digest d, std::uint64_t n) noexcept
            : pid(p), start_generation(g), credential_digest(d), session_nonce(n) {}
    };
    static inline std::mutex channel_mutex_;
    static inline std::map<ChannelId, ChannelEntry> channels_;
    static inline std::uint64_t next_channel_id_ = 1;  // 0 reserved; never wraps into reuse

    static std::variant<ChannelId, AuthError> allocate_channel_id() noexcept {
        try {
            std::scoped_lock guard{channel_mutex_};
            while (next_channel_id_ != 0) {
                ChannelId id{next_channel_id_++};
                if (channels_.find(id) == channels_.end()) return id;
            }
            return AuthError{AuthErrorCode::CHANNEL_ID_EXHAUSTED};
        } catch (...) { return AuthError{AuthErrorCode::ALLOCATION_FAILED}; }
    }
    static ProcessPrincipal process_principal_of(const PeerAuthFacts& f) noexcept {
        return ProcessPrincipal{ProcessIdentity{f.pid, f.start_generation}};
    }
    static AuthStatus register_connection(ChannelId id, const PeerAuthFacts& f) noexcept {
        if (!id.valid()) return auth_err(AuthErrorCode::CHANNEL_ID_EXHAUSTED);
        try {
            ChannelEntry entry{f.start_generation, f.credential_digest, f.session_nonce};
            std::scoped_lock guard{channel_mutex_};
            if (channels_.find(id) != channels_.end())
                return auth_err(AuthErrorCode::CHANNEL_DUPLICATE);
            channels_.emplace(id, entry);
            return auth_ok();
        } catch (...) {
            return auth_err(AuthErrorCode::ALLOCATION_FAILED);
        }
    }
    static AuthStatus revoke_connection(ChannelId id) noexcept {
        try {
            std::scoped_lock guard{channel_mutex_};
            return channels_.erase(id) != 0 ? auth_ok() : auth_err(AuthErrorCode::UNKNOWN_CHANNEL);
        } catch (...) { return auth_err(AuthErrorCode::ALLOCATION_FAILED); }
    }
    static std::optional<AuthenticatedExecutorContext> issue_context(
        ChannelId id, const PeerAuthFacts& f) noexcept {
        try {
            {
                std::scoped_lock guard{channel_mutex_};
                if (channels_.find(id) == channels_.end()) return std::nullopt;
            }
            return AuthenticatedExecutorContext{ExecutorPrincipal{process_principal_of(f)},
                                                AuthenticatedChannel{id, f.start_generation},
                                                f.session_nonce, f.credential_digest};
        } catch (...) { return std::nullopt; }
    }
};

}  // namespace recovery
#endif
