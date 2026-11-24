// G19: StoreDomainId and LinuxBootId do not interchange.
// EXPECT-DIAG: conversion from .*StoreDomainId.* to non-scalar type .*LinuxBootId
#include "recovery/ingress.hpp"

recovery::LinuxBootId forbidden(const recovery::StoreDomainId& id) {
    recovery::LinuxBootId converted = id;
    return converted;
}
