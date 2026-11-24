#include "recovery/runtime.hpp"
#include "test_support.hpp"

#include <fcntl.h>
#include <type_traits>
#include <unistd.h>

RECOVERY_TEST(owned_fd_is_move_only_and_closes_exactly_once) {
    static_assert(!std::is_copy_constructible_v<recovery::OwnedFd>);
    static_assert(std::is_nothrow_move_constructible_v<recovery::OwnedFd>);
    int pipefd[2];
    RECOVERY_CHECK(::pipe(pipefd) == 0);
    const int owned = pipefd[0];
    {
        recovery::OwnedFd first{owned};
        recovery::OwnedFd second{std::move(first)};
        RECOVERY_CHECK(!first.valid());
        RECOVERY_CHECK(second.valid());
    }
    RECOVERY_CHECK(::fcntl(owned, F_GETFD) == -1);
    ::close(pipefd[1]);
}

RECOVERY_TEST(R163_descriptor_registry_mismatches_are_rejected) {
    using namespace recovery;
    const ProcessExitBindingDescriptor descriptor{{{1, 2, 3}, 4, 0}, {10, 20}};
    const std::vector<ProcessExitBindingDescriptor> descriptors{descriptor};
    {
        ProcessExitBindingRegistry registry;
        registry.emplace(descriptor.id, OwnedProcessExitBinding{descriptor, OwnedFd{-1}});
        RECOVERY_CHECK(!descriptors_registry_agree(descriptors, registry));
    }
    {
        int fds[2]; RECOVERY_CHECK(::pipe(fds) == 0);
        auto wrong = descriptor; wrong.id.binding_seq = 1;
        ProcessExitBindingRegistry registry;
        registry.emplace(descriptor.id, OwnedProcessExitBinding{wrong, OwnedFd{fds[0]}});
        RECOVERY_CHECK(!descriptors_registry_agree(descriptors, registry));
        ::close(fds[1]);
    }
    {
        int fds[2]; RECOVERY_CHECK(::pipe(fds) == 0);
        auto wrong = descriptor; wrong.process.start_generation++;
        ProcessExitBindingRegistry registry;
        registry.emplace(descriptor.id, OwnedProcessExitBinding{wrong, OwnedFd{fds[0]}});
        RECOVERY_CHECK(!descriptors_registry_agree(descriptors, registry));
        ::close(fds[1]);
    }
    {
        int first[2]; int second[2];
        RECOVERY_CHECK(::pipe(first) == 0); RECOVERY_CHECK(::pipe(second) == 0);
        ProcessExitBindingRegistry registry;
        registry.emplace(descriptor.id, OwnedProcessExitBinding{descriptor, OwnedFd{first[0]}});
        auto extra = descriptor; extra.id.binding_seq = 1; extra.process.pid++;
        registry.emplace(extra.id, OwnedProcessExitBinding{extra, OwnedFd{second[0]}});
        RECOVERY_CHECK(!descriptors_registry_agree(descriptors, registry));
        ::close(first[1]); ::close(second[1]);
    }
}
