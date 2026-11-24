// G11: AuthorizationDeliveryId does not convert into FaultDeliveryId.
// EXPECT-DIAG: no match for .operator=|cannot convert|no viable
#include "recovery/ingress.hpp"

void forbidden(recovery::FaultDeliveryId& fault, const recovery::AuthorizationDeliveryId& auth) {
    fault = auth;
}
