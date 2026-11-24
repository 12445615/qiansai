// EXPECT-DIAG: no matching function for call to .canonical_write
#include "recovery/runtime.hpp"

void forbidden(recovery::CanonicalWriter& writer) {
    recovery::OwnedProcessExitBinding owned{
        recovery::ProcessExitBindingDescriptor{
            recovery::ProcessExitBindingId{recovery::RecoveryInstanceId{1, 2, 3}, 4, 5},
            recovery::ProcessIdentity{6, 7}},
        recovery::OwnedFd{-1}};
    canonical_write(writer, owned);
}
