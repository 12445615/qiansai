#include "recovery/runtime.hpp"
#include "test_support.hpp"

namespace recovery {

bool RecoveryTestAccess::report_fault(RecoveryCoordinator& value, const FaultReport& report) {
    return value.report_fault(report) == FaultReportResult::ACCEPTED;
}

void RecoveryTestAccess::clear_fault_batch(RecoveryCoordinator& value) {
    value.slot_.active->fault_batch.clear();
}

void RecoveryTestAccess::set_fault_batch(RecoveryCoordinator& value,
                                         std::vector<FaultFact> batch) {
    value.slot_.active->fault_batch = std::move(batch);
}

void RecoveryTestAccess::set_fault_seq_counter(RecoveryCoordinator& value,
                                               std::uint64_t counter) {
    value.slot_.fault_seq_counter = counter;
}

std::uint64_t RecoveryTestAccess::fault_seq_counter(
    const RecoveryCoordinator& value) {
    return value.slot_.fault_seq_counter;
}

}  // namespace recovery

namespace {

struct ThrowAt final : recovery::RuntimeFailureProvider {
    recovery::RuntimeFailureStage target;
    explicit ThrowAt(recovery::RuntimeFailureStage value) : target(value) {}
    void checkpoint(recovery::RuntimeFailureStage value) override {
        if (value == target) throw std::bad_alloc{};
    }
};

recovery::FaultReport report() {
    return {recovery::FaultKind::PACKET_DOMAIN_FAULT,
            {42, std::nullopt, std::nullopt},
            {1, 2, 3, 4},
            {9, {1, 2, 3, 4}}};
}

RECOVERY_TEST(R165_identical_reports_are_two_new_kernel_facts) {
    recovery::RecoveryCoordinator coordinator{7};
    RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(coordinator, report()));
    RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(coordinator, report()));
    RECOVERY_CHECK_EQ(coordinator.active().fault_batch.size(), 2U);
    RECOVERY_CHECK_EQ(coordinator.active().fault_batch[0].id.boot_id, 7U);
    RECOVERY_CHECK_EQ(coordinator.active().fault_batch[0].id.fault_seq, 1U);
    RECOVERY_CHECK_EQ(coordinator.active().fault_batch[1].id.fault_seq, 2U);
    RECOVERY_CHECK(coordinator.active().fault_batch[0].id
                   != coordinator.active().fault_batch[1].id);
    RECOVERY_CHECK(recovery::test::succeeded(coordinator.begin_freeze({})));
    RECOVERY_CHECK(coordinator.commit_freeze());
    RECOVERY_CHECK_EQ(coordinator.active().current_snapshot->fault_batch,
                      coordinator.active().fault_batch);
    RECOVERY_CHECK_EQ(coordinator.active().current_snapshot->fault_batch_cutoff, 2U);
    RECOVERY_CHECK_EQ(coordinator.active().current_snapshot->fault_batch_digest,
                      recovery::canonical_digest(coordinator.active().fault_batch));
}

RECOVERY_TEST(R165_frozen_snapshot_is_unchanged_when_identical_report_is_rejected) {
    recovery::RecoveryCoordinator coordinator{7};
    RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(coordinator, report()));
    RECOVERY_CHECK(recovery::test::succeeded(coordinator.begin_freeze({})));
    RECOVERY_CHECK(coordinator.commit_freeze());
    const auto snapshot = *coordinator.active().current_snapshot;
    RECOVERY_CHECK(!recovery::RecoveryTestAccess::report_fault(coordinator, report()));
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK(coordinator.active().current_snapshot.has_value());
    const auto& published = *coordinator.active().current_snapshot;
    RECOVERY_CHECK_EQ(published.fault_batch, snapshot.fault_batch);
    RECOVERY_CHECK_EQ(published.fault_batch_cutoff, snapshot.fault_batch_cutoff);
    RECOVERY_CHECK_EQ(published.fault_batch_digest, snapshot.fault_batch_digest);
}

RECOVERY_TEST(R165_empty_batch_fails_closed_without_publication) {
    recovery::RecoveryCoordinator coordinator{7};
    RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(coordinator, report()));
    const auto original_counter = recovery::RecoveryTestAccess::fault_seq_counter(coordinator);
    recovery::RecoveryTestAccess::clear_fault_batch(coordinator);
    RECOVERY_CHECK(recovery::test::succeeded(coordinator.begin_freeze({})));
    RECOVERY_CHECK(!coordinator.commit_freeze());
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK(coordinator.active().fault_batch.empty());
    RECOVERY_CHECK_EQ(coordinator.active().current_snapshot.has_value(), false);
    RECOVERY_CHECK_EQ(coordinator.active().process_exit_binding_registry.size(), 0U);
    RECOVERY_CHECK_EQ(recovery::RecoveryTestAccess::fault_seq_counter(coordinator),
                      original_counter);
}

RECOVERY_TEST(R165_duplicate_identity_in_collecting_batch_fails_closed) {
    recovery::RecoveryCoordinator coordinator{7};
    RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(coordinator, report()));
    const auto first = recovery::RecoveryTestAccess::make_fault(7, 1, report());
    const auto duplicate = recovery::RecoveryTestAccess::make_fault(7, 1, report());
    recovery::RecoveryTestAccess::set_fault_batch(coordinator, {first, duplicate});
    RECOVERY_CHECK(recovery::test::succeeded(coordinator.begin_freeze({})));
    RECOVERY_CHECK(!coordinator.commit_freeze());
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK(!coordinator.active().current_snapshot.has_value());
    RECOVERY_CHECK_EQ(coordinator.active().fault_batch.size(), 2U);
    RECOVERY_CHECK_EQ(coordinator.active().fault_batch[0].id.fault_seq, 1U);
    RECOVERY_CHECK_EQ(coordinator.active().fault_batch[1].id.fault_seq, 1U);
}

RECOVERY_TEST(R165_phase_a_failures_preserve_batch_counter_and_publication) {
    using Stage = recovery::RuntimeFailureStage;
    const Stage stages[] = {
        Stage::FREEZE_TARGETS, Stage::FREEZE_OWNER_ACK,
        Stage::FREEZE_BATCH_PREPARE, Stage::FREEZE_PREFIX_DIGEST,
        Stage::FREEZE_BATCH_DIGEST, Stage::FREEZE_AFTER_BINDINGS,
        Stage::FREEZE_BINDING_LEGALITY, Stage::FREEZE_SNAPSHOT,
        Stage::FREEZE_SNAPSHOT_DIGEST};
    for (const auto stage : stages) {
        ThrowAt failure{stage};
        recovery::RecoveryCoordinator coordinator{7, nullptr, &failure};
        RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(coordinator, report()));
        const auto batch = coordinator.active().fault_batch;
        const auto counter = recovery::RecoveryTestAccess::fault_seq_counter(coordinator);
        RECOVERY_CHECK(recovery::test::succeeded(coordinator.begin_freeze({})));
        RECOVERY_CHECK(!coordinator.commit_freeze());
        RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FAIL_CLOSED);
        RECOVERY_CHECK_EQ(coordinator.active().fault_batch, batch);
        RECOVERY_CHECK_EQ(recovery::RecoveryTestAccess::fault_seq_counter(coordinator),
                          counter);
        RECOVERY_CHECK(!coordinator.active().current_snapshot.has_value());
        RECOVERY_CHECK(coordinator.active().process_exit_binding_registry.empty());
    }
}

RECOVERY_TEST(R165_failed_append_preserves_first_fact_and_counter) {
    ThrowAt failure{recovery::RuntimeFailureStage::REPORT_APPEND};
    recovery::RecoveryCoordinator coordinator{7, nullptr, &failure};
    RECOVERY_CHECK(recovery::RecoveryTestAccess::report_fault(coordinator, report()));
    const auto batch = coordinator.active().fault_batch;
    const auto counter = recovery::RecoveryTestAccess::fault_seq_counter(coordinator);
    RECOVERY_CHECK(!recovery::RecoveryTestAccess::report_fault(coordinator, report()));
    RECOVERY_CHECK_EQ(coordinator.state(), recovery::RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK_EQ(coordinator.active().fault_batch, batch);
    RECOVERY_CHECK_EQ(recovery::RecoveryTestAccess::fault_seq_counter(coordinator), counter);
}

}  // namespace
