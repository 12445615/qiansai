// G11: report_fault is private; production code outside TrustedFaultIngress cannot call it.
// EXPECT-DIAG: report_fault.*is private within this context
#include "recovery/runtime.hpp"

bool forbidden(recovery::RecoveryCoordinator& coordinator, const recovery::FaultReport& report) {
    return coordinator.report_fault(report) == recovery::FaultReportResult::ACCEPTED;
}
