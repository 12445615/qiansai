// G19: a LinuxBootId never narrows to uint64_t.
// EXPECT-DIAG: invalid .static_cast.
#include <cstdint>
#include "recovery/ingress.hpp"

std::uint64_t forbidden(const recovery::LinuxBootId& id) {
    return static_cast<std::uint64_t>(id);
}
