// Freeze entry guarantees: R166 (begin_freeze, §6.1) and the R137 lock clause
// of commit_freeze (§6.3).
#include "recovery/runtime.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <limits>
#include <new>
#include <stdexcept>
#include <system_error>
#include <sys/wait.h>
#include <unistd.h>

namespace recovery {

FreezeSeq RecoveryTestAccess::freeze_seq(const RecoveryCoordinator& value) {
    return value.slot_.freeze_seq;
}

void RecoveryTestAccess::set_freeze_seq(RecoveryCoordinator& value, FreezeSeq sequence) {
    value.slot_.freeze_seq = sequence;
}

}  // namespace recovery

namespace {
using recovery::BeginFreezeErrorCode;
using recovery::RecoverySlotState;
using recovery::RuntimeFailureStage;
using Access = recovery::RecoveryTestAccess;

template <typename Exception>
struct ThrowAt final : recovery::RuntimeFailureProvider {
    RuntimeFailureStage target;
    explicit ThrowAt(RuntimeFailureStage value) : target(value) {}
    void checkpoint(RuntimeFailureStage value) override {
        if (value != target) return;
        if constexpr (std::is_same_v<Exception, std::bad_alloc>) throw std::bad_alloc{};
        else if constexpr (std::is_same_v<Exception, std::system_error>)
            throw std::system_error{std::make_error_code(std::errc::resource_deadlock_would_occur)};
        else throw Exception{"injected"};
    }
};

recovery::FaultReport fault() {
    return {recovery::FaultKind::PACKET_DOMAIN_FAULT,
            {1, std::nullopt, std::nullopt}, {1, 2, 3, 4}, {1, {}}};
}

const std::vector<recovery::FrozenGrantRecord>& ledger() {
    static const std::vector<recovery::FrozenGrantRecord> value{
        {1, std::nullopt, {}, {}, {11, 0, 1}, 7, std::nullopt}};
    return value;
}

const std::vector<recovery::OwnerExecutorBinding>& bindings() {
    static const std::vector<recovery::OwnerExecutorBinding> value{
        {1, recovery::ExecutorBindingStatus::RESOLVED,
         recovery::ExecutorPrincipal{recovery::ProcessPrincipal{{99, 1}}}, 1}};
    return value;
}

// Every runtime field begin_freeze may publish, compared byte for byte.
struct FreezeView {
    RecoverySlotState state;
    recovery::FreezeSeq freeze_seq;
    recovery::GrantSeq grant_cutoff;
    recovery::Digest ledger_prefix_bytes;
    recovery::Digest ledger_prefix_digest;
    std::size_t owner_binding_count;
    bool operator==(const FreezeView&) const = default;
};

FreezeView view(const recovery::RecoveryCoordinator& c) {
    return {c.state(), Access::freeze_seq(c), c.active().grant_cutoff,
            recovery::canonical_digest(c.active().ledger_prefix),
            c.active().ledger_prefix_digest, c.active().owner_executor_bindings.size()};
}

std::optional<BeginFreezeErrorCode> error_of(const recovery::BeginFreezeResult& result) {
    if (const auto* error = std::get_if<recovery::BeginFreezeError>(&result)) return error->code;
    return std::nullopt;
}

constexpr RuntimeFailureStage kPhaseAStages[] = {
    RuntimeFailureStage::BEGIN_FREEZE_PREPARE,
    RuntimeFailureStage::BEGIN_FREEZE_AFTER_COPY,
    RuntimeFailureStage::BEGIN_FREEZE_AFTER_DIGEST};

template <typename Runtime>
concept KeepsSecondRequestCopy = requires(Runtime& runtime) { runtime.freeze_request; };
}  // namespace

// ①② prefix snapshot / prefix digest allocation failure.
RECOVERY_TEST(R166_allocation_failure_is_explicit_and_leaves_runtime_unchanged) {
    for (const auto stage : kPhaseAStages) {
        ThrowAt<std::bad_alloc> failure{stage};
        recovery::RecoveryCoordinator c{11, nullptr, &failure};
        RECOVERY_CHECK(c.report_fault(fault()));
        const auto before = view(c);
        const auto result = c.begin_freeze(ledger(), bindings());
        RECOVERY_CHECK(error_of(result) == BeginFreezeErrorCode::ALLOCATION_FAILED);
        RECOVERY_CHECK(view(c) == before);
        RECOVERY_CHECK_EQ(c.state(), RecoverySlotState::PREPARING);
    }
}

// ③ a non-allocation exception is not retryable: FAIL_CLOSED, nothing published.
RECOVERY_TEST(R166_non_allocation_exception_fails_closed_without_publication) {
    for (const auto stage : kPhaseAStages) {
        ThrowAt<std::runtime_error> failure{stage};
        recovery::RecoveryCoordinator c{11, nullptr, &failure};
        RECOVERY_CHECK(c.report_fault(fault()));
        const auto before = view(c);
        const auto result = c.begin_freeze(ledger(), bindings());
        RECOVERY_CHECK(error_of(result) == BeginFreezeErrorCode::INTERNAL_FAILURE);
        RECOVERY_CHECK_EQ(c.state(), RecoverySlotState::FAIL_CLOSED);
        auto unchanged = before;
        unchanged.state = RecoverySlotState::FAIL_CLOSED;
        RECOVERY_CHECK(view(c) == unchanged);
    }
}

// ④ freeze_seq == UINT64_MAX must not wrap to a reused sequence.
RECOVERY_TEST(R166_exhausted_freeze_sequence_fails_closed_without_wrapping) {
    recovery::RecoveryCoordinator c{11};
    RECOVERY_CHECK(c.report_fault(fault()));
    Access::set_freeze_seq(c, std::numeric_limits<recovery::FreezeSeq>::max());
    const auto before = view(c);
    const auto result = c.begin_freeze(ledger(), bindings());
    RECOVERY_CHECK(error_of(result) == BeginFreezeErrorCode::SEQUENCE_EXHAUSTED);
    RECOVERY_CHECK_EQ(c.state(), RecoverySlotState::FAIL_CLOSED);
    auto unchanged = before;
    unchanged.state = RecoverySlotState::FAIL_CLOSED;
    RECOVERY_CHECK(view(c) == unchanged);
}

// ⑤ wrong phase is an explicit error and writes nothing.
RECOVERY_TEST(R166_wrong_phase_is_explicit_and_writes_nothing) {
    {
        recovery::RecoveryCoordinator idle{11};
        const auto result = idle.begin_freeze(ledger(), bindings());
        RECOVERY_CHECK(error_of(result) == BeginFreezeErrorCode::WRONG_PHASE);
        RECOVERY_CHECK_EQ(idle.state(), RecoverySlotState::IDLE);
        RECOVERY_CHECK_EQ(Access::freeze_seq(idle), 0U);
    }
    {
        recovery::RecoveryCoordinator frozen{11};
        RECOVERY_CHECK(frozen.report_fault(fault()));
        RECOVERY_CHECK(recovery::test::succeeded(frozen.begin_freeze({})));
        RECOVERY_CHECK(frozen.commit_freeze());
        const auto sequence = Access::freeze_seq(frozen);
        const auto result = frozen.begin_freeze(ledger(), bindings());
        RECOVERY_CHECK(error_of(result) == BeginFreezeErrorCode::WRONG_PHASE);
        RECOVERY_CHECK_EQ(frozen.state(), RecoverySlotState::FROZEN);
        RECOVERY_CHECK_EQ(Access::freeze_seq(frozen), sequence);
    }
}

// ⑥ one commit; the returned request is the only copy and is what gets re-sent.
RECOVERY_TEST(R166_success_returns_the_committed_request_and_resend_reuses_it) {
    recovery::RecoveryCoordinator c{11};
    RECOVERY_CHECK(c.report_fault(fault()));
    const auto sequence_before = Access::freeze_seq(c);
    const auto result = c.begin_freeze(ledger(), bindings());
    RECOVERY_CHECK(recovery::test::succeeded(result));
    const auto request = std::get<recovery::FreezeRequest>(result);
    RECOVERY_CHECK_EQ(c.state(), RecoverySlotState::FREEZING);
    RECOVERY_CHECK_EQ(Access::freeze_seq(c), sequence_before + 1);
    RECOVERY_CHECK(request.recovery_id == c.active().recovery_id);
    RECOVERY_CHECK_EQ(request.freeze_seq, Access::freeze_seq(c));
    RECOVERY_CHECK_EQ(request.boot_id, 11U);
    RECOVERY_CHECK_EQ(request.grant_cutoff, c.active().grant_cutoff);
    RECOVERY_CHECK(request.ledger_prefix_digest == c.active().ledger_prefix_digest);

    // A partial broadcast re-sends the same value; asking again mints nothing.
    const auto resend = request;
    RECOVERY_CHECK(resend == request);
    const auto again = c.begin_freeze(ledger(), bindings());
    RECOVERY_CHECK(error_of(again) == BeginFreezeErrorCode::WRONG_PHASE);
    RECOVERY_CHECK_EQ(Access::freeze_seq(c), request.freeze_seq);

    // §6.1: the runtime does not keep a second copy of the request.
    RECOVERY_CHECK(!KeepsSecondRequestCopy<recovery::RecoveryInstanceRuntime>);
}

// Runs `body` in a child; the child must end through the watchdog stub.
template <typename Body>
void expect_watchdog_exit(Body body) {
    std::fflush(nullptr);
    const pid_t child = ::fork();
    RECOVERY_CHECK(child >= 0);
    if (child == 0) {
        body();
        ::_exit(0);  // reaching here means the lock failure was never observed
    }
    int status = 0;
    RECOVERY_CHECK_EQ(::waitpid(child, &status, 0), child);
    RECOVERY_CHECK(!WIFSIGNALED(status));  // SIGABRT here is std::terminate
    RECOVERY_CHECK(WIFEXITED(status));
    RECOVERY_CHECK_EQ(WEXITSTATUS(status), recovery::kRecoveryTestWatchdogExitCode);
}

// R166 ⑦ lock-acquire failure reaches only the noreturn watchdog, never std::terminate.
RECOVERY_TEST(R166_lock_failure_exits_through_watchdog_not_terminate) {
    expect_watchdog_exit([] {
        ThrowAt<std::system_error> failure{RuntimeFailureStage::BEGIN_FREEZE_LOCK};
        recovery::RecoveryCoordinator c{11, nullptr, &failure};
        if (!c.report_fault(fault())) ::_exit(2);
        (void)c.begin_freeze(ledger(), bindings());
    });
}

// R137: commit_freeze lock-acquire failure has the same single landing point.
RECOVERY_TEST(R137_commit_freeze_lock_failure_exits_through_watchdog_not_terminate) {
    expect_watchdog_exit([] {
        ThrowAt<std::system_error> failure{RuntimeFailureStage::FREEZE_LOCK};
        recovery::RecoveryCoordinator c{11, nullptr, &failure};
        if (!c.report_fault(fault())) ::_exit(2);
        if (!recovery::test::succeeded(c.begin_freeze({}))) ::_exit(3);
        (void)c.commit_freeze();
    });
}
