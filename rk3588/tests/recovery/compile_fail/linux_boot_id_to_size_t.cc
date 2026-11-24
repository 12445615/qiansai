// G19: a LinuxBootId never narrows to size_t.
// EXPECT-DIAG: cannot convert|could not convert
#include <cstddef>
#include "recovery/ingress.hpp"

std::size_t forbidden(const recovery::LinuxBootId& id) {
    return id;
}
