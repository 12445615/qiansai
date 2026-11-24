#include "recovery/runtime.hpp"
#include "test_support.hpp"

#include <fcntl.h>
#include <unistd.h>

namespace {
struct FakeKernel final : recovery::ProcessExitKernel {
    std::vector<std::optional<std::uint64_t>> reads;
    std::size_t read_index{};
    int fd{-1};
    FakeKernel(std::vector<std::optional<std::uint64_t>> values, int opened_fd)
        : reads(std::move(values)), fd(opened_fd) {}
    std::optional<std::uint64_t> read_start_generation(recovery::ProcessId) override {
        return reads.at(read_index++);
    }
    recovery::OwnedFd pidfd_open(recovery::ProcessId) override {
        return recovery::OwnedFd{fd};
    }
};
struct ThrowAfterBindings final : recovery::RuntimeFailureProvider {
    void checkpoint(recovery::RuntimeFailureStage stage) override {
        if (stage == recovery::RuntimeFailureStage::FREEZE_AFTER_BINDINGS)
            throw std::bad_alloc{};
    }
};
recovery::FaultReport process_fault(recovery::ProcessIdentity process) {
    return {recovery::FaultKind::WORKER_PROCESS_EXIT,
            {42, std::nullopt, process}, {1, 2, 3, 4}, {1, {}}};
}
std::vector<recovery::OwnerExecutorBinding> owners() {
    return {{42, recovery::ExecutorBindingStatus::RESOLVED,
             recovery::ExecutorPrincipal{recovery::ProcessPrincipal{{99, 1}}}, 1}};
}
}

RECOVERY_TEST(process_exit_binding_reads_generation_exactly_twice_and_publishes_atomically) {
    int pipefd[2]; RECOVERY_CHECK(::pipe(pipefd) == 0);
    FakeKernel kernel{{123, 123}, pipefd[0]};
    recovery::RecoveryCoordinator coordinator{9, &kernel};
    RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(coordinator, process_fault({44, 123})));
    RECOVERY_CHECK(recovery::test::succeeded(coordinator.begin_freeze({}, owners())));
    RECOVERY_CHECK(coordinator.commit_freeze());
    RECOVERY_CHECK_EQ(kernel.read_index, 2U);
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FROZEN);
    RECOVERY_CHECK_EQ(coordinator.active().current_snapshot->process_exit_bindings.size(), 1U);
    RECOVERY_CHECK_EQ(coordinator.active().process_exit_binding_registry.size(), 1U);
    ::close(pipefd[1]);
}

RECOVERY_TEST(process_exit_binding_five_failures_fail_closed_without_publication) {
    const std::vector<std::vector<std::optional<std::uint64_t>>> cases = {
        {std::nullopt}, {122}, {123}, {123, std::nullopt}, {123, 122}};
    for (std::size_t index = 0; index < cases.size(); ++index) {
        int pipefd[2]; RECOVERY_CHECK(::pipe(pipefd) == 0);
        FakeKernel kernel{cases[index], index == 2 ? -1 : pipefd[0]};
        recovery::RecoveryCoordinator coordinator{9, &kernel};
        RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(coordinator, process_fault({44, 123})));
        const auto batch = coordinator.active().fault_batch;
        const auto counter = recovery::RecoveryTestAccess::fault_seq_counter(coordinator);
        RECOVERY_CHECK(recovery::test::succeeded(coordinator.begin_freeze({}, owners())));
        RECOVERY_CHECK(!coordinator.commit_freeze());
        RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FAIL_CLOSED);
        RECOVERY_CHECK(!coordinator.active().current_snapshot.has_value());
        RECOVERY_CHECK(coordinator.active().process_exit_binding_registry.empty());
        RECOVERY_CHECK_EQ(coordinator.active().fault_batch, batch);
        RECOVERY_CHECK_EQ(recovery::RecoveryTestAccess::fault_seq_counter(coordinator),
                          counter);
        if (index <= 2) {
            ::close(pipefd[0]);  // pidfd_open was never accepted by the runtime.
        } else {
            RECOVERY_CHECK(::fcntl(pipefd[0], F_GETFD) == -1);
        }
        ::close(pipefd[1]);
    }
}

RECOVERY_TEST(R9_R163_raw_fd_value_does_not_change_frozen_canonical_bytes) {
    int first_pipe[2]; int second_pipe[2];
    RECOVERY_CHECK(::pipe(first_pipe) == 0);
    RECOVERY_CHECK(::pipe(second_pipe) == 0);
    FakeKernel first_kernel{{123, 123}, first_pipe[0]};
    FakeKernel second_kernel{{123, 123}, second_pipe[0]};
    recovery::RecoveryCoordinator first{9, &first_kernel};
    recovery::RecoveryCoordinator second{9, &second_kernel};
    RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(first, process_fault({44, 123})));
    RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(second, process_fault({44, 123})));
    RECOVERY_CHECK(recovery::test::succeeded(first.begin_freeze({}, owners())));
    RECOVERY_CHECK(recovery::test::succeeded(second.begin_freeze({}, owners())));
    RECOVERY_CHECK(first.commit_freeze());
    RECOVERY_CHECK(second.commit_freeze());
    RECOVERY_CHECK_EQ(recovery::canonical_bytes(*first.active().current_snapshot),
                      recovery::canonical_bytes(*second.active().current_snapshot));
    RECOVERY_CHECK(!first.commit_freeze());
    RECOVERY_CHECK_EQ(first.state(), recovery::RecoverySlotState::FAIL_CLOSED);
    ::close(first_pipe[1]); ::close(second_pipe[1]);
}

RECOVERY_TEST(R137_failure_after_pidfd_open_closes_local_fd_without_publication) {
    int pipefd[2]; RECOVERY_CHECK(::pipe(pipefd) == 0);
    const int opened = pipefd[0];
    FakeKernel kernel{{123, 123}, opened};
    ThrowAfterBindings failure;
    recovery::RecoveryCoordinator coordinator{9, &kernel, &failure};
    RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(coordinator, process_fault({44, 123})));
    const auto batch = coordinator.active().fault_batch;
    const auto counter = recovery::RecoveryTestAccess::fault_seq_counter(coordinator);
    RECOVERY_CHECK(recovery::test::succeeded(coordinator.begin_freeze({}, owners())));
    RECOVERY_CHECK(!coordinator.commit_freeze());
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK(!coordinator.active().current_snapshot.has_value());
    RECOVERY_CHECK(coordinator.active().process_exit_binding_registry.empty());
    RECOVERY_CHECK_EQ(coordinator.active().fault_batch, batch);
    RECOVERY_CHECK_EQ(recovery::RecoveryTestAccess::fault_seq_counter(coordinator), counter);
    RECOVERY_CHECK(::fcntl(opened, F_GETFD) == -1);
    ::close(pipefd[1]);
}
