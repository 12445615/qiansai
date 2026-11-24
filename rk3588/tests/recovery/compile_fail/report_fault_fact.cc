// EXPECT-DIAG: cannot convert.*FaultFact.*FaultReport
#include "recovery/runtime.hpp"

void forbidden(recovery::RecoveryCoordinator& coordinator,
               const recovery::FaultFact& fact) {
    coordinator.report_fault(fact);
}
