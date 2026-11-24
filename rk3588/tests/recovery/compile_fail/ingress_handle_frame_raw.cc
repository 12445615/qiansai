// G1: handle_frame takes only an AuthorizedFaultFrame; a raw FaultFrame does
// not convert, even with private access through the test seam.
// EXPECT-DIAG: cannot convert .*FaultFrame.* to .*const recovery::AuthorizedFaultFrame&
#define RECOVERY_TESTING
#include "recovery/ingress.hpp"

namespace recovery {
struct RecoveryTestAccess {
    static FrameOutcome bypass(TrustedFaultIngress& ingress, FaultIngressSession& session,
                               const FaultIngressSessionIo& io, const FaultFrame& raw) {
        return ingress.handle_frame(raw, session, io);
    }
};
}  // namespace recovery
