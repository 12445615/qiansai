// EXPECT-DIAG: has not been declared
#include "recovery/runtime.hpp"

void forbidden(recovery::RecoveryCoordinator& coordinator) {
    recovery::RecoveryTestAccess::make_fault(1, 1, recovery::FaultReport{});
    (void)coordinator;
}
