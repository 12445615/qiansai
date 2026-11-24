// R167: lock and exception boundary of every coordinator migration (§4.4
// lock template; §9.1 begin_compile, §9.2 finish_compile, §9.4 seal). The
// §10.2 begin_quiesce and §14.3 submit_proof_record clauses wait for those
// entry points to exist.
#include "recovery/runtime.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

namespace recovery {

bool RecoveryTestAccess::mutex_free(RecoveryCoordinator& value) {
    bool acquired = false;
    std::thread probe([&] {
        if (value.mutex_.try_lock()) {
            acquired = true;
            value.mutex_.unlock();
        }
    });
    probe.join();
    return acquired;
}

}  // namespace recovery

namespace {
using recovery::RecoverySlotState;
using recovery::RuntimeFailureStage;
using Access = recovery::RecoveryTestAccess;

enum class Throw { BAD_ALLOC, RUNTIME_ERROR, SYSTEM_ERROR };

struct Injector final : recovery::RuntimeFailureProvider {
    RuntimeFailureStage target;
    Throw kind;
    bool enabled{false};
    Injector(RuntimeFailureStage stage, Throw value) : target(stage), kind(value) {}
    void checkpoint(RuntimeFailureStage stage) override {
        if (!enabled || stage != target) return;
        switch (kind) {
        case Throw::BAD_ALLOC: throw std::bad_alloc{};
        case Throw::RUNTIME_ERROR: throw std::runtime_error{"injected"};
        case Throw::SYSTEM_ERROR:
            throw std::system_error{std::make_error_code(std::errc::resource_deadlock_would_occur)};
        }
    }
};

recovery::FaultReport fault() {
    return {recovery::FaultKind::PACKET_DOMAIN_FAULT,
            {1, std::nullopt, std::nullopt}, {1, 2, 3, 4}, {1, {}}};
}

struct Compiling {
    std::unique_ptr<recovery::RecoveryCoordinator> coordinator;
    recovery::PolicySnapshot policy;
    recovery::CapabilityRegistrySnapshot registry;
};

recovery::PolicySnapshot packet_policy() {
    auto policy = std::make_shared<recovery::PolicyContents>();
    policy->dispositions[recovery::FaultKind::PACKET_DOMAIN_FAULT] =
        recovery::FaultDisposition::COVERED;
    policy->recipes[{recovery::ResourceKind::PACKET_DOMAIN, recovery::RecoveryMode::PACKET_RESET}] =
        {recovery::ResourceKind::PACKET_DOMAIN, recovery::RecoveryMode::PACKET_RESET,
         {{recovery::ActionPhase::TEARDOWN, recovery::OperationKind::PACKET_STOP, {}},
          {recovery::ActionPhase::INVALIDATE_EPOCH, recovery::OperationKind::INVALIDATE_EPOCH, {0}},
          {recovery::ActionPhase::REINIT, recovery::OperationKind::REINIT, {1}}}};
    return {2, recovery::canonical_digest(*policy), policy};
}

recovery::CapabilityRegistrySnapshot empty_registry() {
    auto registry = std::make_shared<recovery::CapabilityRegistryContents>();
    return {3, recovery::canonical_digest(*registry), registry};
}

// Drives a coordinator to the state just before the migration under test.
enum class Upto { IDLE, FREEZING, FROZEN, COMPILING, INSTALLED };

Compiling drive(Upto upto, recovery::RuntimeFailureProvider* provider) {
    Compiling in{std::make_unique<recovery::RecoveryCoordinator>(11, nullptr, provider),
                 packet_policy(), empty_registry()};
    if (upto == Upto::IDLE) return in;
    RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(*in.coordinator, fault()));
    RECOVERY_CHECK(recovery::test::succeeded(in.coordinator->begin_freeze({},
        {{1, recovery::ExecutorBindingStatus::RESOLVED,
          recovery::ExecutorPrincipal{recovery::ProcessPrincipal{{99, 100}}}, 1}})));
    if (upto == Upto::FREEZING) return in;
    RECOVERY_CHECK(in.coordinator->commit_freeze());
    if (upto == Upto::FROZEN) return in;
    RECOVERY_CHECK(in.coordinator->begin_compile(in.policy, in.registry));
    if (upto == Upto::COMPILING) return in;
    in.coordinator->finish_compile(recovery::RecoveryCompiler::compile(
        *in.coordinator->active().current_snapshot, in.policy, in.registry, 0));
    RECOVERY_CHECK(in.coordinator->active().compiled_program != nullptr);
    return in;
}

// Runs `body` in a child process and returns its wait status, so a
// std::terminate inside a noexcept entry point is observed, not fatal.
int in_child(const std::function<int()>& body) {
    std::fflush(nullptr);
    const pid_t child = ::fork();
    if (child == 0) {
        int code = 1;
        try { code = body(); } catch (...) { code = 3; }
        ::_exit(code);
    }
    int status = 0;
    RECOVERY_CHECK(child > 0);
    RECOVERY_CHECK_EQ(::waitpid(child, &status, 0), child);
    return status;
}

void expect_clean_exit(int status, int code) {
    RECOVERY_CHECK(!WIFSIGNALED(status));  // SIGABRT = std::terminate
    RECOVERY_CHECK(WIFEXITED(status));
    RECOVERY_CHECK_EQ(WEXITSTATUS(status), code);
}

recovery::OwnerSnapshotAck any_ack(const recovery::RecoveryCoordinator& c) {
    return {c.active().recovery_id, 1, 11, 0, 1, {}, {}, {}};
}
}  // namespace

// ① lock-acquire failure in each migration ends only in the watchdog stub.
RECOVERY_TEST(R167_lock_failure_in_every_migration_exits_through_watchdog) {
    struct Case {
        RuntimeFailureStage stage;
        Upto upto;
        std::function<void(Compiling&)> call;
    };
    const Case cases[] = {
        {RuntimeFailureStage::REPORT_LOCK, Upto::IDLE,
         [](Compiling& in) { (void)recovery::RecoveryTestAccess::report_fault(*in.coordinator, fault()); }},
        {RuntimeFailureStage::OWNER_ACK_LOCK, Upto::FREEZING,
         [](Compiling& in) {
             const auto ctx = Access::issue_test_context({99, 100});
             (void)in.coordinator->submit_owner_snapshot_ack(ctx, any_ack(*in.coordinator));
         }},
        {RuntimeFailureStage::BEGIN_COMPILE_LOCK, Upto::FROZEN,
         [](Compiling& in) { (void)in.coordinator->begin_compile(in.policy, in.registry); }},
        {RuntimeFailureStage::FINISH_COMPILE_LOCK, Upto::COMPILING,
         [](Compiling& in) {
             in.coordinator->finish_compile(recovery::CompileFailure{
                 recovery::CompileFailure::Kind::FALLBACK_REQUIRED, std::nullopt});
         }},
        {RuntimeFailureStage::FALLBACK_LOCK, Upto::COMPILING,
         [](Compiling& in) { in.coordinator->execute_fallback(std::nullopt); }},
        {RuntimeFailureStage::SEAL_LOCK, Upto::INSTALLED,
         [](Compiling& in) {
             (void)in.coordinator->seal_validated_program(
                 in.coordinator->active().compiled_program);
         }},
    };
    for (const auto& c : cases) {
        const auto status = in_child([&c] {
            Injector failure{c.stage, Throw::SYSTEM_ERROR};
            auto in = drive(c.upto, &failure);
            failure.enabled = true;
            c.call(in);
            return 0;  // reaching here means the lock failure was never observed
        });
        expect_clean_exit(status, recovery::kWatchdogRebootExitCode);
    }
}

// ②③④ an exception inside begin_compile / finish_compile / seal goes to
// FAIL_CLOSED, publishes nothing and does not leave the interface.
RECOVERY_TEST(R167_exceptions_in_compile_and_seal_fail_closed_without_publication) {
    for (const auto kind : {Throw::BAD_ALLOC, Throw::RUNTIME_ERROR}) {
        expect_clean_exit(in_child([kind] {
            Injector failure{RuntimeFailureStage::BEGIN_COMPILE_PUBLISH, kind};
            auto in = drive(Upto::FROZEN, &failure);
            failure.enabled = true;
            const bool ok = in.coordinator->begin_compile(in.policy, in.registry);
            const auto& rt = in.coordinator->active();
            return !ok && in.coordinator->state() == RecoverySlotState::FAIL_CLOSED
                && rt.current_policy.contents == nullptr
                && rt.current_registry.contents == nullptr ? 0 : 1;
        }), 0);
        expect_clean_exit(in_child([kind] {
            Injector failure{RuntimeFailureStage::FINISH_COMPILE_INSTALL, kind};
            auto in = drive(Upto::COMPILING, &failure);
            failure.enabled = true;
            in.coordinator->finish_compile(recovery::RecoveryCompiler::compile(
                *in.coordinator->active().current_snapshot, in.policy, in.registry, 0));
            return in.coordinator->state() == RecoverySlotState::FAIL_CLOSED
                && in.coordinator->active().compiled_program == nullptr ? 0 : 1;
        }), 0);
        expect_clean_exit(in_child([kind] {
            Injector failure{RuntimeFailureStage::SEAL_VERIFY, kind};
            auto in = drive(Upto::INSTALLED, &failure);
            failure.enabled = true;
            const bool ok = in.coordinator->seal_validated_program(
                in.coordinator->active().compiled_program);
            const auto& rt = in.coordinator->active();
            return !ok && in.coordinator->state() == RecoverySlotState::FAIL_CLOSED
                && rt.current_program == nullptr
                && rt.transaction_hash == recovery::TransactionHash{} ? 0 : 1;
        }), 0);
    }
}

// ⑥ no return path keeps the coordinator mutex.
RECOVERY_TEST(R167_every_return_path_releases_the_mutex) {
    const auto released = [](recovery::RecoveryCoordinator& c) {
        RECOVERY_CHECK(Access::mutex_free(c));
    };
    {
        auto in = drive(Upto::IDLE, nullptr);
        auto& c = *in.coordinator;
        RECOVERY_CHECK(!recovery::test::succeeded(c.begin_freeze({}))); released(c);
        RECOVERY_CHECK(!c.commit_freeze()); released(c);
        RECOVERY_CHECK(!c.begin_compile()); released(c);       // wrong state → FAIL_CLOSED
        RECOVERY_CHECK(!recovery::RecoveryTestAccess::report_fault(c, fault())); released(c);  // already FAIL_CLOSED
    }
    {
        auto in = drive(Upto::FREEZING, nullptr);
        auto& c = *in.coordinator;
        const auto ctx = Access::issue_test_context({99, 100});
        RECOVERY_CHECK(std::holds_alternative<recovery::OwnerAckError>(
            c.submit_owner_snapshot_ack(ctx, any_ack(c))));
        released(c);
        c.finish_compile(recovery::CompileFailure{
            recovery::CompileFailure::Kind::FALLBACK_REQUIRED, std::nullopt});
        released(c);
    }
    {
        auto in = drive(Upto::COMPILING, nullptr);
        auto& c = *in.coordinator;
        RECOVERY_CHECK(!c.seal_validated_program(nullptr)); released(c);
    }
    {
        auto in = drive(Upto::COMPILING, nullptr);
        auto& c = *in.coordinator;
        c.execute_fallback(std::nullopt); released(c);
    }
    {
        auto in = drive(Upto::INSTALLED, nullptr);
        auto& c = *in.coordinator;
        RECOVERY_CHECK(c.seal_validated_program(c.active().compiled_program)); released(c);
        RECOVERY_CHECK_EQ(c.state(), RecoverySlotState::SEALED);
    }
}
