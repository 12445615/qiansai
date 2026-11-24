// G11: FaultDeliveryId does not convert into AuthorizationDeliveryId.
// EXPECT-DIAG: no match for .operator=|cannot convert|no viable
#include "recovery/ingress.hpp"

void forbidden(recovery::AuthorizationDeliveryId& auth, const recovery::FaultDeliveryId& fault) {
    auth = fault;
}
