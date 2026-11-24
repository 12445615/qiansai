// EXPECT-DIAG: canonical variant index exceeds one byte
#include "recovery/canonical.hpp"

void forbidden() {
    recovery::detail::validate_variant_alternative_count<257>();
}
