#include <csignal>
#include <cstdlib>
#include <limits>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "recovery/proof.hpp"
#include "test_support.hpp"

namespace {

recovery::OwnedProcessExitBinding binding_for(pid_t pid) {
    const recovery::ProcessIdentity process{static_cast<recovery::ProcessId>(pid), 77};
    const recovery::ProcessExitBindingId id{{5, 6, 7}, 8, 0};
    return {recovery::ProcessExitBindingDescriptor{id, process},
            recovery::OwnedFd{static_cast<int>(::syscall(SYS_pidfd_open, pid, 0))}};
}

void reap(pid_t pid) {
    int status = 0;
    RECOVERY_CHECK(::waitpid(pid, &status, 0) == pid);
}

}  // namespace

RECOVERY_TEST(R164_closed_pidfd_has_no_trusted_observation) {
    const recovery::ProcessExitBindingDescriptor descriptor{{{5, 6, 7}, 8, 0}, {1, 77}};
    const recovery::OwnedProcessExitBinding owned{descriptor, recovery::OwnedFd{}};
    RECOVERY_CHECK(!recovery::observe_process_exit_trusted(owned).has_value());
}

RECOVERY_TEST(R164_poll_error_is_unknown_with_errno) {
    const recovery::ProcessExitBindingDescriptor descriptor{{{5, 6, 7}, 8, 0}, {1, 77}};
    const recovery::OwnedProcessExitBinding owned{
        descriptor, recovery::OwnedFd{std::numeric_limits<int>::max()}};
    const auto observed = recovery::observe_process_exit_trusted(owned);
    RECOVERY_CHECK(observed.has_value());
    RECOVERY_CHECK_EQ(observed->wait_code, recovery::WaitCode::UNKNOWN);
    RECOVERY_CHECK_EQ(observed->wait_errno, EBADF);
    RECOVERY_CHECK(!observed->exited);
}

RECOVERY_TEST(R164_not_yet_exited_is_unknown_and_not_terminated) {
    const pid_t child = ::fork();
    RECOVERY_CHECK(child >= 0);
    if (child == 0) {
        for (;;) ::pause();
    }

    auto owned = binding_for(child);
    RECOVERY_CHECK(owned.pidfd.valid());
    const auto observed = recovery::observe_process_exit_trusted(owned);
    RECOVERY_CHECK(observed.has_value());
    RECOVERY_CHECK_EQ(observed->wait_code, recovery::WaitCode::UNKNOWN);
    RECOVERY_CHECK(!observed->exited);

    RECOVERY_CHECK(::kill(child, SIGKILL) == 0);
    reap(child);
}

RECOVERY_TEST(R164_normal_exit_is_trusted_without_reaping) {
    const pid_t child = ::fork();
    RECOVERY_CHECK(child >= 0);
    if (child == 0) ::_exit(0);

    auto owned = binding_for(child);
    RECOVERY_CHECK(owned.pidfd.valid());
    std::optional<recovery::TrustedProcessExitObservation> observed;
    for (int attempt = 0; attempt != 100 && (!observed || !observed->exited); ++attempt) {
        observed = recovery::observe_process_exit_trusted(owned);
        if (!observed->exited) ::usleep(1000);
    }
    RECOVERY_CHECK(observed.has_value());
    RECOVERY_CHECK(observed->exited);
    RECOVERY_CHECK_EQ(observed->wait_code, recovery::WaitCode::EXITED);
    RECOVERY_CHECK_EQ(observed->wait_errno, 0);
    reap(child);
}

RECOVERY_TEST(R164_signal_exit_is_trusted_as_signaled) {
    const pid_t child = ::fork();
    RECOVERY_CHECK(child >= 0);
    if (child == 0) {
        for (;;) ::pause();
    }

    auto owned = binding_for(child);
    RECOVERY_CHECK(owned.pidfd.valid());
    RECOVERY_CHECK(::kill(child, SIGTERM) == 0);
    std::optional<recovery::TrustedProcessExitObservation> observed;
    for (int attempt = 0; attempt != 100 && (!observed || !observed->exited); ++attempt) {
        observed = recovery::observe_process_exit_trusted(owned);
        if (!observed->exited) ::usleep(1000);
    }
    RECOVERY_CHECK(observed.has_value() && observed->exited);
    RECOVERY_CHECK_EQ(observed->wait_code, recovery::WaitCode::SIGNALED);
    reap(child);
}
