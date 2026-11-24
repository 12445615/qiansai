// EXPECT-DIAG: private within this context
#include "recovery/program.hpp"

void forbidden(recovery::BootId boot) {
    recovery::FaultIdentity id{boot, 1};
    (void)id;
}
