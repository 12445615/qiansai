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
recovery::FrozenGrantRecord grant(recovery::OwnerId owner) {
    return {42, std::nullopt, {}, {}, {7, 0, 1}, owner, 99};
}
recovery::OwnerSnapshotAck ack_for(const recovery::RecoveryCoordinator& coordinator,
                                   recovery::OwnerId owner) {
    recovery::OwnerSnapshotAck ack{coordinator.active().recovery_id, 1, 7, 0, owner,
                                   {{7, 0, 1}}, {99}, {}};
    ack.local_view_digest = recovery::canonical_digest(ack);
    return ack;
}

RECOVERY_TEST(R11_report_fault_and_commit_freeze_are_mutex_linearized) {
    for (unsigned round = 0; round < 100; ++round) {
        recovery::RecoveryCoordinator coordinator{7};
        RECOVERY_CHECK(coordinator.report_fault(fault(1)));
        RECOVERY_CHECK(coordinator.begin_freeze({}, owners()));
        std::thread reporter([&] { (void)coordinator.report_fault(fault(2)); });
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

RECOVERY_TEST(R40_missing_owner_ack_fails_closed_without_snapshot) {
    recovery::RecoveryCoordinator coordinator{7};
    RECOVERY_CHECK(coordinator.report_fault(fault(1)));
    RECOVERY_CHECK(coordinator.begin_freeze({grant(55)}));
    RECOVERY_CHECK(!coordinator.commit_freeze());
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK(!coordinator.active().current_snapshot.has_value());
    RECOVERY_CHECK(coordinator.active().process_exit_binding_registry.empty());
}

RECOVERY_TEST(R40_owner_snapshot_ack_exact_validation_and_complete_set) {
    recovery::RecoveryCoordinator coordinator{7};
    RECOVERY_CHECK(coordinator.report_fault(fault(1)));
    RECOVERY_CHECK(coordinator.begin_freeze({grant(55)}, owners()));
    const auto valid = ack_for(coordinator, 55);
    const auto rejected = [&](auto mutate) {
        auto candidate = valid;
        mutate(candidate);
        candidate.local_view_digest = {};
        candidate.local_view_digest = recovery::canonical_digest(candidate);
        RECOVERY_CHECK(!coordinator.submit_owner_snapshot_ack(candidate));
    };
    rejected([](auto& a) { a.recovery_id.nonce++; });
    rejected([](auto& a) { a.freeze_seq++; });
    rejected([](auto& a) { a.boot_id++; });
    rejected([](auto& a) { a.closing_generation++; });
    rejected([](auto& a) { a.owner++; });
    rejected([](auto& a) { a.locally_observed_grants.clear(); });
    rejected([](auto& a) { a.locally_observed_grants.insert({7, 0, 2}); });
    rejected([](auto& a) { a.outstanding_leases.clear(); });
    auto bad_digest = valid; bad_digest.local_view_digest[0] ^= 1U;
    RECOVERY_CHECK(!coordinator.submit_owner_snapshot_ack(bad_digest));
    RECOVERY_CHECK(coordinator.submit_owner_snapshot_ack(valid));
    RECOVERY_CHECK(!coordinator.submit_owner_snapshot_ack(valid));
    RECOVERY_CHECK(coordinator.commit_freeze());
    RECOVERY_CHECK_EQ(coordinator.active().current_snapshot->owner_ack_canonical_digest,
                      coordinator.active().owner_ack_canonical_digest);
}
RECOVERY_TEST(R10_R14_fault_batch_is_canonical_and_closed) {
    recovery::RecoveryCoordinator coordinator{7};
    RECOVERY_CHECK(coordinator.report_fault(fault(3)));
    RECOVERY_CHECK(coordinator.report_fault(fault(1)));
    RECOVERY_CHECK(coordinator.report_fault(fault(3)));
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::PREPARING);
    RECOVERY_CHECK_EQ(coordinator.active().fault_batch.size(), 3U);
    RECOVERY_CHECK_EQ(coordinator.active().fault_batch[0].id.fault_seq, 1U);
    RECOVERY_CHECK_EQ(coordinator.active().fault_batch[1].id.fault_seq, 2U);
    RECOVERY_CHECK_EQ(coordinator.active().fault_batch[2].id.fault_seq, 3U);
    RECOVERY_CHECK(coordinator.begin_freeze({}, owners()));
    RECOVERY_CHECK(coordinator.commit_freeze());
    RECOVERY_CHECK_EQ(coordinator.active().current_snapshot->fault_batch,
                      coordinator.active().fault_batch);
    RECOVERY_CHECK_EQ(coordinator.active().current_snapshot->fault_batch_cutoff, 3U);
    RECOVERY_CHECK_EQ(coordinator.active().current_snapshot->fault_batch_digest,
                      recovery::canonical_digest(coordinator.active().fault_batch));
    RECOVERY_CHECK(!coordinator.report_fault(fault(4)));
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FAIL_CLOSED);
}
}
