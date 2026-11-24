// G19: CoordinatorBootId and StoreDomainId do not interchange.
// EXPECT-DIAG: conversion from .*CoordinatorBootId.* to non-scalar type .*StoreDomainId
#include "recovery/ingress.hpp"

recovery::StoreDomainId forbidden(const recovery::CoordinatorBootId& id) {
    recovery::StoreDomainId converted = id;
    return converted;
}
