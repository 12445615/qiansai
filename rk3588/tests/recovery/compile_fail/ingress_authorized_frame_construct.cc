// G1: only the session reader may mint an AuthorizedFaultFrame.
// EXPECT-DIAG: AuthorizedFaultFrame.*is private within this context
#include "recovery/ingress.hpp"

recovery::AuthorizedFaultFrame forge(recovery::FaultFrame frame) {
    return recovery::AuthorizedFaultFrame{std::move(frame)};
}
