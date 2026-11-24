// Task 4F evidence gaps against the frozen baseline: R165 ③⑤⑦a⑦c⑦f⑦g⑩,
// R137 non-allocation exceptions in Phase A, and R163 ③ byte-identical
// re-commit rejection.
#include "recovery/runtime.hpp"
#include "test_support.hpp"

#include <limits>
#include <stdexcept>
#include <unistd.h>

namespace recovery {

void RecoveryTestAccess::flip_ledger_prefix_digest(RecoveryCoordinator& value) {
    value.slot_.active->ledger_prefix_digest[0] ^= 1U;
}

void RecoveryTestAccess::set_recovery_seq(RecoveryCoordinator& value, RecoverySeq sequence) {
    value.slot_.recovery_seq = sequence;
}

}  // namespace recovery

namespace {
using recovery::RecoverySlotState;
using recovery::RuntimeFailureStage;
using Access = recovery::RecoveryTestAccess;

recovery::FaultReport report(std::uint64_t evidence = 9) {
    return {recovery::FaultKind::PACKET_DOMAIN_FAULT,
            {42, std::nullopt, std::nullopt}, {1, 2, 3, 4}, {evidence, {1, 2, 3, 4}}};
}

struct RuntimeErrorAt final : recovery::RuntimeFailureProvider {
    RuntimeFailureStage target;
    explicit RuntimeErrorAt(RuntimeFailureStage stage) : target(stage) {}
    void checkpoint(RuntimeFailureStage stage) override {
        if (stage == target) throw std::runtime_error{"injected"};
    }
};

// Asserts the R165 ③ outcome of a failed commit: FAIL_CLOSED, batch and
// counter byte-identical, nothing published.
void expect_unpublished(const recovery::RecoveryCoordinator& c,
                        const std::vector<recovery::FaultFact>& batch, std::uint64_t counter) {
    RECOVERY_CHECK_EQ(c.state(), RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK_EQ(c.active().fault_batch, batch);
    RECOVERY_CHECK_EQ(Access::fault_seq_counter(c), counter);
    RECOVERY_CHECK(!c.active().current_snapshot.has_value());
    RECOVERY_CHECK(c.active().process_exit_binding_registry.empty());
}

recovery::PolicySnapshot packet_policy() {
    auto policy = std::make_shared<recovery::PolicyContents>();
    policy->dispositions[recovery::FaultKind::PACKET_DOMAIN_FAULT] =
        recovery::FaultDisposition::COVERED;
    policy->recipes[{recovery::ResourceKind::PACKET_DOMAIN, recovery::RecoveryMode::PACKET_RESET}] =
        {recovery::ResourceKind::PACKET_DOMAIN, recovery::RecoveryMode::PACKET_RESET,
         {{recovery::ActionPhase::TEARDOWN, recovery::OperationKind::PACKET_STOP, {}},
          {recovery::ActionPhase::INVALIDATE_EPOCH, recovery::OperationKind::INVALIDATE_EPOCH, {0}},
          {recovery::ActionPhase::REINIT, recovery::OperationKind::REINIT, {1}}}};
    return {2, recovery::canonical_digest(*policy), policy};
}
}  // namespace

// ⑦a counter 0 → first fault is 1 and freezes with cutoff 1; ⑦c every seq ≤ cutoff, last == cutoff.
RECOVERY_TEST(R165_first_fault_is_one_and_cutoff_bounds_the_batch) {
    recovery::RecoveryCoordinator c{7};
    RECOVERY_CHECK_EQ(Access::fault_seq_counter(c), 0U);
    RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(c, report()));
    RECOVERY_CHECK(recovery::test::succeeded(c.begin_freeze({})));
    RECOVERY_CHECK(c.commit_freeze());
    const auto& snapshot = *c.active().current_snapshot;
    RECOVERY_CHECK_EQ(snapshot.fault_batch.size(), 1U);
    RECOVERY_CHECK_EQ(snapshot.fault_batch.front().id.fault_seq, 1U);
    RECOVERY_CHECK_EQ(snapshot.fault_batch_cutoff, 1U);

    recovery::RecoveryCoordinator three{7};
    for (std::uint64_t e = 1; e <= 3; ++e) RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(three, report(e)));
    RECOVERY_CHECK(recovery::test::succeeded(three.begin_freeze({})));
    RECOVERY_CHECK(three.commit_freeze());
    const auto& frozen = *three.active().current_snapshot;
    for (const auto& fact : frozen.fault_batch)
        RECOVERY_CHECK(fact.id.fault_seq <= frozen.fault_batch_cutoff);
    RECOVERY_CHECK_EQ(frozen.fault_batch.back().id.fault_seq, frozen.fault_batch_cutoff);
}

// ⑤ SEALED: a byte-identical report still fails closed; the frozen triple is unchanged.
RECOVERY_TEST(R165_identical_report_after_seal_fails_closed_with_snapshot_unchanged) {
    recovery::RecoveryCoordinator c{7};
    RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(c, report()));
    RECOVERY_CHECK(recovery::test::succeeded(c.begin_freeze({},
        {{42, recovery::ExecutorBindingStatus::RESOLVED,
          recovery::ExecutorPrincipal{recovery::ProcessPrincipal{{99, 100}}}, 1}})));
    RECOVERY_CHECK(c.commit_freeze());
    auto registry_contents = std::make_shared<recovery::CapabilityRegistryContents>();
    const recovery::CapabilityRegistrySnapshot registry{
        3, recovery::canonical_digest(*registry_contents), registry_contents};
    const auto policy = packet_policy();
    RECOVERY_CHECK(c.begin_compile(policy, registry));
    c.finish_compile(recovery::RecoveryCompiler::compile(
        *c.active().current_snapshot, policy, registry, 0));
    RECOVERY_CHECK(c.seal_validated_program(c.active().compiled_program));
    RECOVERY_CHECK_EQ(c.state(), RecoverySlotState::SEALED);
    const auto before = *c.active().current_snapshot;
    RECOVERY_CHECK(!recovery::RecoveryTestAccess::report_fault(c, report()));
    RECOVERY_CHECK_EQ(c.state(), RecoverySlotState::FAIL_CLOSED);
    const auto& after = *c.active().current_snapshot;
    RECOVERY_CHECK_EQ(after.fault_batch, before.fault_batch);
    RECOVERY_CHECK_EQ(after.fault_batch_cutoff, before.fault_batch_cutoff);
    RECOVERY_CHECK(after.fault_batch_digest == before.fault_batch_digest);
}

// ⑦f(b) one identity carried by two different facts is malformed: no snapshot.
RECOVERY_TEST(R165_same_identity_different_content_fails_closed) {
    recovery::RecoveryCoordinator c{7};
    RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(c, report()));
    const auto first = Access::make_fault(7, 1, report(1));
    const auto other = Access::make_fault(7, 1, report(2));
    Access::set_fault_batch(c, {first, other});
    const auto batch = c.active().fault_batch;
    const auto counter = Access::fault_seq_counter(c);
    RECOVERY_CHECK(recovery::test::succeeded(c.begin_freeze({})));
    RECOVERY_CHECK(!c.commit_freeze());
    expect_unpublished(c, batch, counter);
}

// ⑦g fault sequence exhaustion never wraps into a reused identity.
RECOVERY_TEST(R165_fault_sequence_exhaustion_fails_closed_without_writes) {
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    {   // appending while PREPARING
        recovery::RecoveryCoordinator c{7};
        RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(c, report()));
        Access::set_fault_seq_counter(c, kMax);
        const auto batch = c.active().fault_batch;
        RECOVERY_CHECK(!recovery::RecoveryTestAccess::report_fault(c, report(2)));
        RECOVERY_CHECK_EQ(c.state(), RecoverySlotState::FAIL_CLOSED);
        RECOVERY_CHECK_EQ(c.active().fault_batch, batch);
        RECOVERY_CHECK_EQ(Access::fault_seq_counter(c), kMax);
    }
    {   // the first fault of a new recovery
        recovery::RecoveryCoordinator c{7};
        Access::set_fault_seq_counter(c, kMax);
        RECOVERY_CHECK(!recovery::RecoveryTestAccess::report_fault(c, report()));
        RECOVERY_CHECK_EQ(c.state(), RecoverySlotState::FAIL_CLOSED);
        RECOVERY_CHECK(!c.has_active());
        RECOVERY_CHECK_EQ(Access::fault_seq_counter(c), kMax);
    }
    {   // recovery sequence exhaustion on the first fault
        recovery::RecoveryCoordinator c{7};
        Access::set_recovery_seq(c, kMax);
        RECOVERY_CHECK(!recovery::RecoveryTestAccess::report_fault(c, report()));
        RECOVERY_CHECK_EQ(c.state(), RecoverySlotState::FAIL_CLOSED);
        RECOVERY_CHECK(!c.has_active());
        RECOVERY_CHECK_EQ(Access::fault_seq_counter(c), 0U);
    }
}

// ⑩ / R137: a non-allocation exception in report_fault or anywhere in Phase A
// fails closed and keeps batch, counter and publication untouched.
RECOVERY_TEST(R165_R137_non_allocation_exceptions_fail_closed_without_writes) {
    {
        RuntimeErrorAt failure{RuntimeFailureStage::REPORT_APPEND};
        recovery::RecoveryCoordinator c{7, nullptr, &failure};
        RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(c, report()));
        const auto batch = c.active().fault_batch;
        const auto counter = Access::fault_seq_counter(c);
        RECOVERY_CHECK(!recovery::RecoveryTestAccess::report_fault(c, report(2)));
        RECOVERY_CHECK_EQ(c.state(), RecoverySlotState::FAIL_CLOSED);
        RECOVERY_CHECK_EQ(c.active().fault_batch, batch);
        RECOVERY_CHECK_EQ(Access::fault_seq_counter(c), counter);
    }
    for (const auto stage : {RuntimeFailureStage::FREEZE_TARGETS, RuntimeFailureStage::FREEZE_PREFIX_DIGEST,
                             RuntimeFailureStage::FREEZE_BATCH_DIGEST, RuntimeFailureStage::FREEZE_AFTER_BINDINGS,
                             RuntimeFailureStage::FREEZE_SNAPSHOT_DIGEST}) {
        RuntimeErrorAt failure{stage};
        recovery::RecoveryCoordinator c{7, nullptr, &failure};
        RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(c, report()));
        const auto batch = c.active().fault_batch;
        const auto counter = Access::fault_seq_counter(c);
        RECOVERY_CHECK(recovery::test::succeeded(c.begin_freeze({})));
        RECOVERY_CHECK(!c.commit_freeze());
        expect_unpublished(c, batch, counter);
    }
}

// ③ the remaining named Phase A failure points: prefix digest mismatch and a missing owner ACK.
RECOVERY_TEST(R165_R137_prefix_digest_mismatch_and_missing_ack_publish_nothing) {
    {
        recovery::RecoveryCoordinator c{7};
        RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(c, report()));
        const auto batch = c.active().fault_batch;
        const auto counter = Access::fault_seq_counter(c);
        RECOVERY_CHECK(recovery::test::succeeded(c.begin_freeze({})));
        Access::flip_ledger_prefix_digest(c);
        RECOVERY_CHECK(!c.commit_freeze());
        expect_unpublished(c, batch, counter);
    }
    {
        recovery::FrozenGrantRecord external{42, std::nullopt, {}, {}, {7, 0, 1}, 55, std::nullopt};
        external.source_executor_principal = recovery::ProcessPrincipal{{99, 1}};
        external.provenance = recovery::GrantProvenance::AUTHENTICATED_EXECUTOR;
        recovery::RecoveryCoordinator c{7};
        RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(c, report()));
        const auto batch = c.active().fault_batch;
        const auto counter = Access::fault_seq_counter(c);
        RECOVERY_CHECK(recovery::test::succeeded(c.begin_freeze({external})));
        RECOVERY_CHECK(!c.commit_freeze());
        expect_unpublished(c, batch, counter);
    }
}

namespace {
struct CountingKernel final : recovery::ProcessExitKernel {
    int fd;
    std::size_t reads{};
    std::size_t opens{};
    explicit CountingKernel(int value) : fd(value) {}
    std::optional<std::uint64_t> read_start_generation(recovery::ProcessId) override {
        ++reads;
        return 123;
    }
    recovery::OwnedFd pidfd_open(recovery::ProcessId) override {
        ++opens;
        return recovery::OwnedFd{fd};
    }
};
}  // namespace

// R163 ③ a second commit after publication fails closed without touching
// the published snapshot or registry and without reopening any fd.
RECOVERY_TEST(R163_recommit_after_publication_changes_nothing) {
    int pipefd[2];
    RECOVERY_CHECK(::pipe(pipefd) == 0);
    CountingKernel kernel{pipefd[0]};
    recovery::RecoveryCoordinator c{9, &kernel};
    RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(c, {recovery::FaultKind::WORKER_PROCESS_EXIT,
                                   {42, std::nullopt, recovery::ProcessIdentity{44, 123}},
                                   {1, 2, 3, 4}, {1, {}}}));
    RECOVERY_CHECK(recovery::test::succeeded(c.begin_freeze({})));
    RECOVERY_CHECK(c.commit_freeze());
    const auto bytes = recovery::canonical_bytes(*c.active().current_snapshot);
    const auto& registry = c.active().process_exit_binding_registry;
    RECOVERY_CHECK_EQ(registry.size(), 1U);
    const int published_fd = registry.begin()->second.pidfd.get();
    const auto reads = kernel.reads;
    const auto opens = kernel.opens;

    RECOVERY_CHECK(!c.commit_freeze());
    RECOVERY_CHECK_EQ(c.state(), RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK_EQ(recovery::canonical_bytes(*c.active().current_snapshot), bytes);
    RECOVERY_CHECK_EQ(c.active().process_exit_binding_registry.size(), 1U);
    RECOVERY_CHECK_EQ(c.active().process_exit_binding_registry.begin()->second.pidfd.get(),
                      published_fd);
    RECOVERY_CHECK_EQ(kernel.reads, reads);
    RECOVERY_CHECK_EQ(kernel.opens, opens);
    ::close(pipefd[1]);
}
