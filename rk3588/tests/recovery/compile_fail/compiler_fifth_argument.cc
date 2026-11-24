// EXPECT-DIAG: no matching function for call to
#include "recovery/compiler.hpp"

void forbidden(const recovery::FrozenRuntimeSnapshot& snapshot,
               const recovery::PolicySnapshot& policy,
               const recovery::CapabilityRegistrySnapshot& registry) {
    recovery::RecoveryCompiler::compile(snapshot, policy, registry, 0, nullptr);
}
