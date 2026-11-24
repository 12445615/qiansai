// G11: fault ingress accepts only its own move-only FaultIngressSession,
// never an executor AuthenticatedChannel.
// EXPECT-DIAG: could not convert|cannot convert|no matching function
#include "recovery/ingress.hpp"

void forbidden(recovery::TrustedFaultIngress& ingress, recovery::AuthenticatedChannel& channel) {
    (void)ingress.handle_session(channel);
}
