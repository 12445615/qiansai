// G11: fail_closed_from_ingress is private to the TrustedFaultIngress friend.
// EXPECT-DIAG: fail_closed_from_ingress.*is private within this context
#include "recovery/runtime.hpp"

void forbidden(recovery::RecoveryCoordinator& coordinator) {
    coordinator.fail_closed_from_ingress(recovery::IngressFailureCode::INTERNAL_FAILURE);
}
