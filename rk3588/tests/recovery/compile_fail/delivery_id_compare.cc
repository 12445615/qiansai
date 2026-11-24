// G11: the two delivery ids cannot be compared.
// EXPECT-DIAG: no match for .operator==
#include "recovery/ingress.hpp"

bool forbidden(const recovery::FaultDeliveryId& fault, const recovery::AuthorizationDeliveryId& auth) {
    return fault == auth;
}
