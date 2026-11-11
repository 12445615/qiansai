#include "recovery/canonical.hpp"
#include "recovery/compiler.hpp"
#include "recovery/crypto.hpp"
#include "recovery/program.hpp"
#include "recovery/types.hpp"

#include <memory>
#include <variant>

int main() {
    const recovery::CanonicalBytes bytes{'a', 'b', 'c'};
    const auto digest = recovery::digest_bytes(bytes);
    recovery::FrozenRuntimeSnapshot snapshot{};
    auto policy_contents = std::make_shared<recovery::PolicyContents>();
    auto registry_contents = std::make_shared<recovery::CapabilityRegistryContents>();
    const recovery::PolicySnapshot policy{
        1, recovery::canonical_digest(*policy_contents), policy_contents};
    const recovery::CapabilityRegistrySnapshot registry{
        1, recovery::canonical_digest(*registry_contents), registry_contents};
    const auto result = recovery::RecoveryCompiler::compile(snapshot, policy, registry, 0);
    return digest[0] == 0xba
        && recovery::process_exit_bindings_are_legal(snapshot)
        && std::holds_alternative<recovery::CompileFailure>(result) ? 0 : 1;
}
