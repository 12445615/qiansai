// G19: LinuxBootId and CoordinatorBootId do not interchange.
// EXPECT-DIAG: conversion from .*LinuxBootId.* to non-scalar type .*CoordinatorBootId
#include "recovery/ingress.hpp"

recovery::CoordinatorBootId forbidden(const recovery::LinuxBootId& id) {
    recovery::CoordinatorBootId converted = id;
    return converted;
}
