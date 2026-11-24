#include "recovery/runtime.hpp"
#include "test_support.hpp"

#include <thread>

namespace {
recovery::FaultReport fault(std::uint64_t evidence_id) {
    return {recovery::FaultKind::PACKET_DOMAIN_FAULT,
            {42, std::nullopt, std::nullopt}, {1, 2, 3, 4}, {evidence_id, {}}};
}
std::vector<recovery::OwnerExecutorBinding> owners() {
    return {{42, recovery::ExecutorBindingStatus::RESOLVED,
             recovery::ExecutorPrincipal{recovery::ProcessPrincipal{{99, 1}}}, 1}};
}
}  // namespace

RECOVERY_TEST(R11_report_fault_and_commit_freeze_are_mutex_linearized) {
    for (unsigned round = 0; round < 100; ++round) {
        recovery::RecoveryCoordinator coordinator{7};
        RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(coordinator, fault(1)));
        RECOVERY_CHECK(recovery::test::succeeded(coordinator.begin_freeze({}, owners())));
        std::thread reporter([&] { (void)recovery::RecoveryTestAccess::report_fault(coordinator, fault(2)); });
        std::thread committer([&] { (void)coordinator.commit_freeze(); });
        reporter.join(); committer.join();
        if (coordinator.state() == recovery::RecoverySlotState::FROZEN) {
            RECOVERY_CHECK_EQ(coordinator.active().fault_batch.size(), 2U);
            RECOVERY_CHECK_EQ(coordinator.active().current_snapshot->fault_batch,
                              coordinator.active().fault_batch);
            RECOVERY_CHECK_EQ(coordinator.active().current_snapshot->fault_batch_cutoff,
                              2U);
            RECOVERY_CHECK_EQ(coordinator.active().current_snapshot->fault_batch_digest,
                              recovery::canonical_digest(coordinator.active().fault_batch));
        } else {
            RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FAIL_CLOSED);
            RECOVERY_CHECK_EQ(coordinator.active().fault_batch.size(), 1U);
        }
    }
}

RECOVERY_TEST(R10_R14_fault_batch_is_canonical_and_closed) {
    recovery::RecoveryCoordinator coordinator{7};
    RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(coordinator, fault(3)));
    RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(coordinator, fault(1)));
    RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(coordinator, fault(3)));
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::PREPARING);
    RECOVERY_CHECK_EQ(coordinator.active().fault_batch.size(), 3U);
    RECOVERY_CHECK_EQ(coordinator.active().fault_batch[0].id.fault_seq, 1U);
    RECOVERY_CHECK_EQ(coordinator.active().fault_batch[1].id.fault_seq, 2U);
    RECOVERY_CHECK_EQ(coordinator.active().fault_batch[2].id.fault_seq, 3U);
    RECOVERY_CHECK(recovery::test::succeeded(coordinator.begin_freeze({}, owners())));
    RECOVERY_CHECK(coordinator.commit_freeze());
    RECOVERY_CHECK_EQ(coordinator.active().current_snapshot->fault_batch,
                      coordinator.active().fault_batch);
    RECOVERY_CHECK_EQ(coordinator.active().current_snapshot->fault_batch_cutoff, 3U);
    RECOVERY_CHECK_EQ(coordinator.active().current_snapshot->fault_batch_digest,
                      recovery::canonical_digest(coordinator.active().fault_batch));
    RECOVERY_CHECK(!recovery::RecoveryTestAccess::report_fault(coordinator, fault(4)));
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FAIL_CLOSED);
}
