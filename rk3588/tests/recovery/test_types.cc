#include "test_support.hpp"

#include "recovery/types.hpp"
#include "permit_test_access.hpp"

#include <type_traits>
#include <utility>
#include <variant>

using namespace recovery;

static_assert(!std::is_same_v<ExecutionPredicateKey, DevicePredicateKey>);
static_assert(!std::is_constructible_v<ExecutionPredicateKey, DevicePredicateKey>);
static_assert(!std::is_copy_constructible_v<PermitHandle>);
static_assert(!std::is_default_constructible_v<PermitHandle>);
static_assert(!std::is_move_assignable_v<PermitHandle>);
static_assert(std::is_move_constructible_v<PermitHandle>);
static_assert(std::variant_size_v<ExecutorPrincipal> == 2);
static_assert(!std::is_constructible_v<ExecutorPrincipal, ThreadIdentity>);

RECOVERY_TEST(R59_typed_predicate_keys_do_not_interchange) {
    const PredicateKey execution = ExecutionPredicateKey{
        ExecutionProofKind::PROCESS_EXIT_CONFIRMED, ProcessIdentity{17, 3}};
    const PredicateKey device = DevicePredicateKey{
        DeviceProofKind::CONTEXT_DESTROY_CONFIRMED, 17};
    RECOVERY_CHECK(execution != device);
}

RECOVERY_TEST(R126_executor_principal_has_only_process_and_coordinator) {
    const ExecutorPrincipal process = ProcessPrincipal{ProcessIdentity{31, 9}};
    const ExecutorPrincipal coordinator = CoordinatorPrincipal{};
    RECOVERY_CHECK(process.index() == 0);
    RECOVERY_CHECK(coordinator.index() == 1);
    RECOVERY_CHECK(process != coordinator);
}

RECOVERY_TEST(R75_permit_binding_distinguishes_recovery_instances) {
    const RecoveryInstanceId first{4, 8, 15};
    const RecoveryInstanceId second{4, 9, 15};
    auto permit = PermitHandleTestAccess::issue(first, TransactionHash{}, 16, 23);
    RECOVERY_CHECK(PermitHandleTestAccess::matches(permit, first, TransactionHash{}, 16, 23));
    RECOVERY_CHECK(!PermitHandleTestAccess::matches(permit, second, TransactionHash{}, 16, 23));
}

RECOVERY_TEST(R76_permit_move_and_consume_invalidate_handle) {
    auto source = PermitHandleTestAccess::issue(
        RecoveryInstanceId{1, 2, 3}, TransactionHash{}, 5, 8);
    PermitHandle destination(std::move(source));
    RECOVERY_CHECK(!source.valid());
    RECOVERY_CHECK(destination.valid());
    PermitHandleTestAccess::consume(destination);
    RECOVERY_CHECK(!destination.valid());
}
