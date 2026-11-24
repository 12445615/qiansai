// EXPECT-DIAG: cannot convert.*DevicePredicateKey.*ExecutionProofKind
#include "recovery/types.hpp"

void forbidden(recovery::DevicePredicateKey device) {
    recovery::ExecutionPredicateKey execution(device); // expected-error: no matching cross-type constructor
    (void)execution;
}
