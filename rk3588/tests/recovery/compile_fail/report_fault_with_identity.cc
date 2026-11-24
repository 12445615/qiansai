// EXPECT-DIAG: no matching function for call to
#include "recovery/runtime.hpp"

void forbidden(recovery::RecoveryCoordinator& coordinator,
               const recovery::FaultReport& report,
               const recovery::FaultIdentity& identity) {
    coordinator.report_fault(report, identity);
}
