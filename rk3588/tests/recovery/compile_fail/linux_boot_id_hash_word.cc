// G19: there is no hash word for a LinuxBootId to be truncated into.
// EXPECT-DIAG: could not convert.*__hash_enum<recovery::LinuxBootId
#include <functional>
#include "recovery/ingress.hpp"

std::size_t forbidden(const recovery::LinuxBootId& id) {
    return std::hash<recovery::LinuxBootId>{}(id);
}
