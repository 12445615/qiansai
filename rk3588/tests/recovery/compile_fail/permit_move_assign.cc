#include "recovery/types.hpp"

#include <utility>

void forbidden(recovery::PermitHandle& destination, recovery::PermitHandle& source) {
    destination = std::move(source); // expected-error: use of deleted move assignment
}
