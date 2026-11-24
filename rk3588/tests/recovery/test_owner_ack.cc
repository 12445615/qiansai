// R40 / R168: owner set derived from the frozen prefix and the single
// authenticated OwnerSnapshotAck intake (§6.1, §6.2, §6.3).
#include "recovery/runtime.hpp"
#include "test_support.hpp"

#include <new>

namespace recovery {

AuthenticatedExecutorContext RecoveryTestAccess::issue_test_context(
    ProcessIdentity process, ChannelId* channel, bool stale_nonce) {
    const auto allocated = ExecutorAuthenticator::allocate_channel_id();
    const auto id = std::get<ChannelId>(allocated);
    Digest credential{};
    credential[0] = static_cast<std::uint8_t>(id.value);
    const ExecutorAuthenticator::PeerAuthFacts facts{
        process.pid, process.start_generation, credential, 1000 + id.value};
    if (!std::holds_alternative<AuthOk>(ExecutorAuthenticator::register_connection(id, facts)))
        throw std::runtime_error{"register_connection failed"};
    if (channel) *channel = id;
    const ExecutorAuthenticator::PeerAuthFacts presented{
        process.pid, process.start_generation, credential,
        stale_nonce ? facts.session_nonce + 1 : facts.session_nonce};
    auto context = ExecutorAuthenticator::issue_context(id, presented);
    if (!context) throw std::runtime_error{"issue_context failed"};
    return std::move(*context);
}

void RecoveryTestAccess::revoke_test_channel(ChannelId id) {
    (void)ExecutorAuthenticator::revoke_connection(id);
}

}  // namespace recovery

namespace {
using recovery::OwnerAckErrorCode;
using recovery::RecoverySlotState;
using Access = recovery::RecoveryTestAccess;

constexpr recovery::OwnerId kExternal = 55;
constexpr recovery::OwnerId kInternal = 77;
const recovery::ProcessIdentity kOwnerProcess{99, 1};
const recovery::ProcessIdentity kOtherProcess{98, 1};

recovery::FaultReport fault() {
    return {recovery::FaultKind::PACKET_DOMAIN_FAULT,
            {42, std::nullopt, std::nullopt}, {1, 2, 3, 4}, {1, {}}};
}

recovery::FrozenGrantRecord external_grant(recovery::GrantSeq seq,
                                           recovery::GrantState state = recovery::GrantState::MATERIALIZED,
                                           recovery::ProcessIdentity principal = kOwnerProcess) {
    recovery::FrozenGrantRecord record{42, std::nullopt, {}, {}, {7, 0, seq}, kExternal, 99};
    record.source_executor_principal = recovery::ProcessPrincipal{principal};
    record.provenance = recovery::GrantProvenance::AUTHENTICATED_EXECUTOR;
    record.state = state;
    return record;
}

recovery::FrozenGrantRecord internal_grant(recovery::GrantSeq seq, recovery::OwnerId owner = kInternal) {
    return {43, std::nullopt, {}, {}, {7, 0, seq}, owner, std::nullopt};
}

// Active external grant 1, RELEASED external grant 3, internal grant 2.
std::vector<recovery::FrozenGrantRecord> mixed_prefix() {
    return {external_grant(1), internal_grant(2),
            external_grant(3, recovery::GrantState::RELEASED)};
}

std::unique_ptr<recovery::RecoveryCoordinator> freezing(
    const std::vector<recovery::FrozenGrantRecord>& prefix,
    recovery::RuntimeFailureProvider* provider = nullptr) {
    auto c = std::make_unique<recovery::RecoveryCoordinator>(7, nullptr, provider);
    RECOVERY_CHECK(c->report_fault(fault()));
    RECOVERY_CHECK(recovery::test::succeeded(c->begin_freeze(prefix)));
    return c;
}

recovery::OwnerSnapshotAck sealed(recovery::OwnerSnapshotAck ack) {
    ack.local_view_digest = recovery::recompute_owner_ack_digest(ack);
    return ack;
}

recovery::OwnerSnapshotAck external_ack(const recovery::RecoveryCoordinator& c) {
    return sealed({c.active().recovery_id, 1, 7, 0, kExternal, {{7, 0, 1}}, {99}, {}});
}

std::optional<OwnerAckErrorCode> error_of(const recovery::OwnerAckResult& result) {
    if (const auto* error = std::get_if<recovery::OwnerAckError>(&result)) return error->code;
    return std::nullopt;
}

std::optional<bool> accepted_duplicate(const recovery::OwnerAckResult& result) {
    if (const auto* ok = std::get_if<recovery::OwnerAckAccepted>(&result)) return ok->duplicate;
    return std::nullopt;
}

struct Throwing final : recovery::RuntimeFailureProvider {
    bool enabled{false};
    void checkpoint(recovery::RuntimeFailureStage stage) override {
        if (enabled && stage == recovery::RuntimeFailureStage::OWNER_ACK_INSERT)
            throw std::bad_alloc{};
    }
};
}  // namespace

RECOVERY_TEST(R168_begin_freeze_derives_required_owners_and_synthesizes_internal_acks) {
    auto c = freezing(mixed_prefix());
    const std::vector<recovery::RequiredOwner> expected{
        {kExternal, recovery::ProcessPrincipal{kOwnerProcess}}, {kInternal, std::nullopt}};
    RECOVERY_CHECK(c->active().required_owners == expected);
    RECOVERY_CHECK_EQ(c->active().accepted_owner_acks.size(), 1U);
    const auto& synthesized = c->active().accepted_owner_acks.at(kInternal);
    const std::set<recovery::GrantIdentity> internal_view{{7, 0, 2}};
    RECOVERY_CHECK(synthesized.locally_observed_grants == internal_view);
    RECOVERY_CHECK(synthesized.outstanding_leases.empty());
    RECOVERY_CHECK(synthesized.local_view_digest == recovery::recompute_owner_ack_digest(synthesized));
}

// ①–⑧: every rejection leaves accepted_owner_acks and state untouched.
RECOVERY_TEST(R168_rejected_acks_return_exact_codes_and_write_nothing) {
    {
        recovery::RecoveryCoordinator preparing{7};
        RECOVERY_CHECK(preparing.report_fault(fault()));
        const auto ctx = Access::issue_test_context(kOwnerProcess);
        const recovery::OwnerSnapshotAck ack{preparing.active().recovery_id, 1, 7, 0, kExternal,
                                             {{7, 0, 1}}, {}, {}};
        RECOVERY_CHECK(error_of(preparing.submit_owner_snapshot_ack(ctx, sealed(ack)))
                       == OwnerAckErrorCode::WRONG_PHASE);
        RECOVERY_CHECK_EQ(preparing.state(), RecoverySlotState::PREPARING);
    }
    auto c = freezing(mixed_prefix());
    const auto before = c->active().accepted_owner_acks;
    const auto unchanged = [&] {
        RECOVERY_CHECK(c->active().accepted_owner_acks == before);
        RECOVERY_CHECK_EQ(c->state(), RecoverySlotState::FREEZING);
    };
    const auto ctx = Access::issue_test_context(kOwnerProcess);
    const auto valid = external_ack(*c);
    const auto expect = [&](const recovery::AuthenticatedExecutorContext& context,
                            const recovery::OwnerSnapshotAck& ack, OwnerAckErrorCode code) {
        RECOVERY_CHECK(error_of(c->submit_owner_snapshot_ack(context, ack)) == code);
        unchanged();
    };
    {   // ② stale context: revoked channel, and a nonce the table never held
        recovery::ChannelId channel{};
        const auto revoked = Access::issue_test_context(kOwnerProcess, &channel);
        Access::revoke_test_channel(channel);
        expect(revoked, valid, OwnerAckErrorCode::UNAUTHORIZED);
        const auto forged = Access::issue_test_context(kOwnerProcess, nullptr, true);
        expect(forged, valid, OwnerAckErrorCode::UNAUTHORIZED);
    }
    const auto mutated = [&](auto change) { auto ack = valid; change(ack); return sealed(ack); };
    // ③ binding to this freeze
    expect(ctx, mutated([](auto& a) { a.recovery_id.nonce++; }), OwnerAckErrorCode::BINDING_MISMATCH);
    expect(ctx, mutated([](auto& a) { a.freeze_seq++; }), OwnerAckErrorCode::BINDING_MISMATCH);
    expect(ctx, mutated([](auto& a) { a.boot_id++; }), OwnerAckErrorCode::BINDING_MISMATCH);
    expect(ctx, mutated([](auto& a) { a.closing_generation++; }), OwnerAckErrorCode::BINDING_MISMATCH);
    // ④ owner outside required_owners
    expect(ctx, mutated([](auto& a) { a.owner = 56; }), OwnerAckErrorCode::NOT_REQUIRED_OWNER);
    // ⑤ owner A's ACK from principal B
    expect(Access::issue_test_context(kOtherProcess), valid, OwnerAckErrorCode::UNAUTHORIZED);
    // ⑥ an external ACK for the internal owner
    expect(ctx, mutated([](auto& a) {
        a.owner = kInternal;
        a.locally_observed_grants = {{7, 0, 2}};
    }), OwnerAckErrorCode::UNAUTHORIZED);
    // ⑦ one grant too many, one too few, a RELEASED grant
    expect(ctx, mutated([](auto& a) { a.locally_observed_grants.insert({7, 0, 9}); }),
           OwnerAckErrorCode::GRANT_VIEW_MISMATCH);
    expect(ctx, mutated([](auto& a) { a.locally_observed_grants.clear(); }),
           OwnerAckErrorCode::GRANT_VIEW_MISMATCH);
    expect(ctx, mutated([](auto& a) { a.locally_observed_grants.insert({7, 0, 3}); }),
           OwnerAckErrorCode::GRANT_VIEW_MISMATCH);
    // ⑧ tampered digest
    auto tampered = valid;
    tampered.local_view_digest[0] ^= 1U;
    expect(ctx, tampered, OwnerAckErrorCode::DIGEST_MISMATCH);
}

// ⑨ identical resend is idempotent; ⑩ a different second ACK is a conflict.
RECOVERY_TEST(R168_duplicate_is_idempotent_and_conflict_fails_closed) {
    auto c = freezing(mixed_prefix());
    const auto ctx = Access::issue_test_context(kOwnerProcess);
    const auto valid = external_ack(*c);
    RECOVERY_CHECK(accepted_duplicate(c->submit_owner_snapshot_ack(ctx, valid)) == false);
    const auto after_first = c->active().accepted_owner_acks;
    RECOVERY_CHECK(accepted_duplicate(c->submit_owner_snapshot_ack(ctx, valid)) == true);
    RECOVERY_CHECK(c->active().accepted_owner_acks == after_first);
    auto different = valid;
    different.outstanding_leases = {100};
    RECOVERY_CHECK(error_of(c->submit_owner_snapshot_ack(ctx, sealed(different)))
                   == OwnerAckErrorCode::CONFLICT);
    RECOVERY_CHECK_EQ(c->state(), RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK(c->active().accepted_owner_acks == after_first);
}

// ⑪ allocation failure on insert writes nothing; the same ACK can be resent.
RECOVERY_TEST(R168_insert_allocation_failure_writes_nothing) {
    Throwing failure;
    auto c = freezing(mixed_prefix(), &failure);
    const auto ctx = Access::issue_test_context(kOwnerProcess);
    const auto before = c->active().accepted_owner_acks;
    failure.enabled = true;
    RECOVERY_CHECK(error_of(c->submit_owner_snapshot_ack(ctx, external_ack(*c)))
                   == OwnerAckErrorCode::ALLOCATION_FAILED);
    RECOVERY_CHECK(c->active().accepted_owner_acks == before);
    RECOVERY_CHECK_EQ(c->state(), RecoverySlotState::FREEZING);
    failure.enabled = false;
    RECOVERY_CHECK(accepted_duplicate(c->submit_owner_snapshot_ack(ctx, external_ack(*c))) == false);
}

// ⑫ mixed provenance or two source principals: no request, FAIL_CLOSED.
RECOVERY_TEST(R168_invalid_owner_binding_fails_begin_freeze_closed) {
    const std::vector<std::vector<recovery::FrozenGrantRecord>> prefixes{
        {external_grant(1), internal_grant(2, kExternal)},
        {external_grant(1), external_grant(2, recovery::GrantState::AUTHORIZED, kOtherProcess)}};
    for (const auto& prefix : prefixes) {
        recovery::RecoveryCoordinator c{7};
        RECOVERY_CHECK(c.report_fault(fault()));
        const auto result = c.begin_freeze(prefix);
        const auto* error = std::get_if<recovery::BeginFreezeError>(&result);
        RECOVERY_CHECK(error != nullptr);
        RECOVERY_CHECK(error->code == recovery::BeginFreezeErrorCode::OWNER_BINDING_INVALID);
        RECOVERY_CHECK_EQ(c.state(), RecoverySlotState::FAIL_CLOSED);
        RECOVERY_CHECK(c.active().required_owners.empty());
    }
}

// ⑬ internal-only prefix freezes without any external ACK.
RECOVERY_TEST(R168_internal_only_prefix_commits_without_external_ack) {
    auto c = freezing({internal_grant(1), internal_grant(2, 78)});
    RECOVERY_CHECK(c->commit_freeze());
    RECOVERY_CHECK_EQ(c->state(), RecoverySlotState::FROZEN);
}

// R40: a required external owner without an ACK cannot freeze ("empty set passes" is impossible).
RECOVERY_TEST(R40_missing_external_owner_ack_fails_closed_without_snapshot) {
    auto c = freezing(mixed_prefix());
    RECOVERY_CHECK(!c->commit_freeze());
    RECOVERY_CHECK_EQ(c->state(), RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK(!c->active().current_snapshot.has_value());
    RECOVERY_CHECK(c->active().process_exit_binding_registry.empty());
}

// Complete set: the snapshot's ACK digest is the canonical digest of accepted_owner_acks.
RECOVERY_TEST(R40_R168_complete_set_publishes_canonical_ack_digest) {
    auto c = freezing(mixed_prefix());
    const auto ctx = Access::issue_test_context(kOwnerProcess);
    RECOVERY_CHECK(accepted_duplicate(c->submit_owner_snapshot_ack(ctx, external_ack(*c))) == false);
    RECOVERY_CHECK(c->commit_freeze());
    RECOVERY_CHECK(c->active().current_snapshot->owner_ack_canonical_digest
                   == recovery::canonical_digest(c->active().accepted_owner_acks));
}
