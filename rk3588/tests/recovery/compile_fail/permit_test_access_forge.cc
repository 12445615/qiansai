// EXPECT-DIAG: is private within this context
#include "recovery/types.hpp"

namespace recovery {
class PermitHandleTestAccess {
public:
    static PermitHandle forge() {
        return PermitHandle(RecoveryInstanceId{1, 2, 3}, TransactionHash{}, 4, 5);
    }
};
}  // namespace recovery
