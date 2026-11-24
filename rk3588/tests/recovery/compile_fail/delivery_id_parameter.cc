// G11: a FaultDeliveryId cannot be passed where an AuthorizationDeliveryId is expected.
// EXPECT-DIAG: could not convert|cannot convert
#include "recovery/ingress.hpp"

void takes_authorization(recovery::AuthorizationDeliveryId);
void forbidden(const recovery::FaultDeliveryId& fault) {
    takes_authorization(fault);
}
