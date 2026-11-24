// G11: the kernel never receives a FaultDeliveryId, even through the test friend.
// EXPECT-DIAG: cannot convert.*FaultDeliveryId.*FaultReport
#define RECOVERY_TESTING
#include "recovery/ingress.hpp"

namespace recovery {
struct RecoveryTestAccess {
    static void forbidden(RecoveryCoordinator& coordinator, const FaultDeliveryId& id) {
        (void)coordinator.report_fault(id);
    }
};
}  // namespace recovery
