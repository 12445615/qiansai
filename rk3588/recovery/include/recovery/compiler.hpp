#ifndef RECOVERY_COMPILER_HPP
#define RECOVERY_COMPILER_HPP

#include "recovery/program.hpp"

namespace recovery {

enum class CompilerAllocationStage : std::uint32_t {
    DERIVE_PLANS, ACTION_CLOSURE, DERIVE_PREDICATES, CONSTRUCT_PROGRAM
};

#ifdef RECOVERY_TESTING
class RecoveryCompilerTestAccess;
#endif

class RecoveryCompiler {
public:
    [[nodiscard]] static CompileResult compile(
        const FrozenRuntimeSnapshot&, const PolicySnapshot&,
        const CapabilityRegistrySnapshot&, TrustedMonotonicTime) noexcept;
private:
    class CompilerAllocationBoundary {
    public:
        virtual ~CompilerAllocationBoundary() = default;
        virtual void checkpoint(CompilerAllocationStage) = 0;
#ifdef RECOVERY_TESTING
        virtual void mutate_predicates_for_test(DerivedPredicates&) {}
#endif
    };
    class NoopCompilerAllocationBoundary final : public CompilerAllocationBoundary {
        void checkpoint(CompilerAllocationStage) override {}
    };
#ifdef RECOVERY_TESTING
    friend class RecoveryCompilerTestAccess;
#endif
    [[nodiscard]] static CompileResult compile_impl(
        const FrozenRuntimeSnapshot&, const PolicySnapshot&,
        const CapabilityRegistrySnapshot&, TrustedMonotonicTime,
        CompilerAllocationBoundary&) noexcept;
};

[[nodiscard]] bool process_exit_bindings_are_legal(
    const FrozenRuntimeSnapshot&) noexcept;
[[nodiscard]] const ProcessExitBindingDescriptor* find_process_exit_binding(
    const FrozenRuntimeSnapshot&, const ProcessIdentity&) noexcept;

[[nodiscard]] ActionClosureResult canonical_action_closure(
    const std::vector<RecoveryResourcePlan>&, const FrozenRuntimeSnapshot&,
    const PolicySnapshot&);

[[nodiscard]] PredicateDerivation derive_predicates(
    const std::vector<RecoveryResourcePlan>&, const FrozenRuntimeSnapshot&,
    const PolicySnapshot&, const CapabilityRegistrySnapshot&, TrustedMonotonicTime);

}  // namespace recovery

#endif
