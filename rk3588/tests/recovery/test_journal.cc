// Task 4I S2: FaultJournalStore wrapper and the file journal (§4.6.5).
// Store-level halves of G3, G10, G13, G16 and G21; the "no kernel call after
// the window closes" halves need the ingress path (S3).
#include "recovery/journal.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace recovery {

DeadlineTicket RecoveryTestAccess::unarmed_ticket() {
    DeadlineTicket ticket{nullptr, nullptr, {1}, IngressDeadlineKind::PREPARED_JOURNAL_IO, {0}};
    ticket.armed_ = false;
    return ticket;
}

void RecoveryTestAccess::fail_next_journal_lock(FaultJournalStore& store) {
    store.fail_next_lock_ = true;
}

bool RecoveryTestAccess::append_raw_journal_entry(FileFaultJournalStore& store,
                                                  const std::vector<std::uint8_t>& payload) {
    if (std::holds_alternative<JournalError>(store.read_observed_generation_impl())) return false;
    return std::holds_alternative<std::monostate>(store.append(payload));
}

}  // namespace recovery

namespace {
using recovery::JournalError;
using recovery::JournalRecordState;
using recovery::JournalWrapperState;
using Access = recovery::RecoveryTestAccess;

const recovery::StoreDomainId kDomain{{0xd0, 0x4a}};

struct DomainAuthority final : recovery::StoreDomainAuthority {
    recovery::StoreDomainId expected{kDomain};
    std::uint64_t floor{0};
    std::size_t advances{0};
    bool fail_advance{false};
    std::variant<std::uint64_t, JournalError> verify_startup(
        recovery::StoreDomainId domain, std::uint64_t observed) noexcept override {
        if (domain != expected || observed != floor) return JournalError::GENERATION_MISMATCH;
        return floor;
    }
    std::variant<std::uint64_t, JournalError> verify_and_advance(
        recovery::StoreDomainId domain, std::uint64_t current) noexcept override {
        if (domain != expected || current != floor) return JournalError::GENERATION_MISMATCH;
        if (fail_advance) return JournalError::AUTHORITY_UNAVAILABLE;
        ++advances;
        return ++floor;
    }
};

struct Epoch final : recovery::RetentionEpochAuthority {
    std::variant<std::uint64_t, JournalError> value;
    explicit Epoch(std::uint64_t v) : value(v) {}
    std::variant<std::uint64_t, JournalError> current_epoch() noexcept override { return value; }
};

struct FailingOps final : recovery::JournalFileOps {
    bool fail_write{false};
    bool short_write{false};
    bool fail_fsync{false};
    ssize_t write(int fd, const void* data, std::size_t size) noexcept override {
        if (fail_write) return -1;
        if (short_write) return recovery::JournalFileOps::write(fd, data, size / 2);
        return recovery::JournalFileOps::write(fd, data, size);
    }
    int fsync(int fd) noexcept override {
        return fail_fsync ? -1 : recovery::JournalFileOps::fsync(fd);
    }
};

struct TempJournal {
    std::string dir;
    std::string path;
    TempJournal() {
        char pattern[] = "/tmp/recovery-journal-XXXXXX";
        const char* made = ::mkdtemp(pattern);
        RECOVERY_CHECK(made != nullptr);
        dir = made;
        path = dir + "/fault.wal";
        RECOVERY_CHECK(std::holds_alternative<std::monostate>(
            recovery::FileFaultJournalStore::initialize(path, kDomain)));
    }
    ~TempJournal() {
        std::remove(path.c_str());
        std::remove((path + ".copy").c_str());
        ::rmdir(dir.c_str());
    }
    std::vector<char> bytes() const {
        std::ifstream in{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{in}, {}};
    }
    void overwrite(const std::vector<char>& data) const {
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
};

struct Opened {
    std::shared_ptr<recovery::FileFaultJournalStore> store;
    std::shared_ptr<Epoch> write_epochs;
};

Opened open_store(const TempJournal& journal, const std::shared_ptr<DomainAuthority>& authority,
                  std::size_t capacity = 16, std::uint64_t retention = 10,
                  std::shared_ptr<recovery::JournalFileOps> ops = nullptr,
                  std::uint64_t write_epoch = 100) {
    auto epochs = std::make_shared<Epoch>(write_epoch);
    auto store = recovery::FileFaultJournalStore::open(
        {journal.path, kDomain, capacity, retention, epochs, std::move(ops)}, authority);
    RECOVERY_CHECK(store != nullptr);
    return {store, epochs};
}

recovery::JournalStartupSnapshot start(recovery::FaultJournalStore& store, std::uint64_t boot,
                                       std::uint64_t epoch = 100) {
    Epoch retention{epoch};
    auto result = store.startup_scan_and_reclaim_old_boot({boot}, retention);
    RECOVERY_CHECK(std::holds_alternative<recovery::JournalStartupSnapshot>(result));
    return std::get<recovery::JournalStartupSnapshot>(std::move(result));
}

std::optional<JournalError> startup_error(recovery::FaultJournalStore& store, std::uint64_t boot,
                                          std::uint64_t epoch = 100) {
    Epoch retention{epoch};
    auto result = store.startup_scan_and_reclaim_old_boot({boot}, retention);
    if (const auto* error = std::get_if<JournalError>(&result)) return *error;
    return std::nullopt;
}

recovery::DedupKey key(std::uint64_t boot, std::uint8_t delivery) {
    recovery::DedupKey value{{boot}, {{}, {7}, {4242, 99}}, {}};
    value.delivery.value[0] = delivery;
    return value;
}

recovery::Digest digest_of(std::uint8_t tag) {
    recovery::Digest value{};
    value[0] = tag;
    return value;
}

std::optional<JournalError> error_of(const recovery::JournalWriteResult& result) {
    if (const auto* error = std::get_if<JournalError>(&result)) return *error;
    return std::nullopt;
}

recovery::JournalWriteResult prepare(recovery::FaultJournalStore& store, const recovery::DedupKey& k,
                                     const recovery::Digest& d) {
    auto reserved = store.reserve_current_boot_record(k);
    RECOVERY_CHECK(std::holds_alternative<recovery::CurrentBootRecordReservation>(reserved));
    return store.write_prepared(std::get<recovery::CurrentBootRecordReservation>(std::move(reserved)),
                                k, d, Access::unarmed_ticket());
}

void deliver(recovery::FaultJournalStore& store, const recovery::DedupKey& k, const recovery::Digest& d) {
    RECOVERY_CHECK(!error_of(prepare(store, k, d)).has_value());
    RECOVERY_CHECK(!error_of(store.write_delivered(k, d, Access::unarmed_ticket())).has_value());
}

// Raw payloads in the journal's own entry layout, for malformed-record samples.
std::vector<std::uint8_t> raw_prepared(std::uint64_t generation, const recovery::DedupKey& k,
                                       bool truncate = false) {
    recovery::CanonicalWriter writer;
    recovery::canonical_write(writer, std::uint8_t{1});
    recovery::canonical_write(writer, generation);
    recovery::canonical_write(writer, k);
    recovery::canonical_write(writer, digest_of(1));
    recovery::canonical_write(writer, std::uint64_t{5});
    if (!truncate) recovery::canonical_write(writer, std::uint64_t{100});
    return std::move(writer).finish();
}
}  // namespace

RECOVERY_TEST(S2_prepared_then_delivered_survives_reopen_in_the_same_boot) {
    TempJournal journal;
    auto authority = std::make_shared<DomainAuthority>();
    {
        auto opened = open_store(journal, authority);
        RECOVERY_CHECK(start(*opened.store, 1).current_boot_records.empty());
        RECOVERY_CHECK_EQ(opened.store->wrapper_state(), JournalWrapperState::READY);
        const auto prepared = prepare(*opened.store, key(1, 1), digest_of(9));
        RECOVERY_CHECK(!error_of(prepared).has_value());
        RECOVERY_CHECK_EQ(std::get<recovery::FaultJournalRecord>(prepared).store_generation, 1U);
        const auto delivered =
            opened.store->write_delivered(key(1, 1), digest_of(9), Access::unarmed_ticket());
        RECOVERY_CHECK(!error_of(delivered).has_value());
        RECOVERY_CHECK_EQ(std::get<recovery::FaultJournalRecord>(delivered).store_generation, 2U);
    }
    // G3 store half: the same CoordinatorBootId reopens with the DELIVERED record visible.
    auto reopened = open_store(journal, authority);
    const auto snapshot = start(*reopened.store, 1);
    RECOVERY_CHECK_EQ(snapshot.current_boot_records.size(), 1U);
    const auto& record = snapshot.current_boot_records.front();
    RECOVERY_CHECK(record.key == key(1, 1));
    RECOVERY_CHECK(record.payload_digest == digest_of(9));
    RECOVERY_CHECK_EQ(record.state, JournalRecordState::DELIVERED);
    RECOVERY_CHECK_EQ(record.store_generation, 2U);
    RECOVERY_CHECK_EQ(record.created_epoch, 100U);
}

// G21: a reservation for key B cannot write key A; rejected before any advance.
RECOVERY_TEST(G21_reservation_key_mismatch_is_rejected_before_advance) {
    TempJournal journal;
    auto authority = std::make_shared<DomainAuthority>();
    auto opened = open_store(journal, authority);
    start(*opened.store, 1);
    auto reserved_b = opened.store->reserve_current_boot_record(key(1, 2));
    RECOVERY_CHECK(std::holds_alternative<recovery::CurrentBootRecordReservation>(reserved_b));
    const auto mismatched = opened.store->write_prepared(
        std::get<recovery::CurrentBootRecordReservation>(std::move(reserved_b)), key(1, 1),
        digest_of(1), Access::unarmed_ticket());
    RECOVERY_CHECK(error_of(mismatched) == JournalError::RESERVATION_KEY_MISMATCH);
    RECOVERY_CHECK_EQ(authority->advances, 0U);
    RECOVERY_CHECK_EQ(opened.store->wrapper_state(), JournalWrapperState::READY);
    RECOVERY_CHECK(!error_of(prepare(*opened.store, key(1, 1), digest_of(1))).has_value());
}

// G21: write_delivered is an exact PREPARED→DELIVERED, never an upsert.
RECOVERY_TEST(G21_write_delivered_counterexamples_poison_without_upsert) {
    enum class Case { NO_PREPARED, ALREADY_DELIVERED, OTHER_KEY, OTHER_DIGEST };
    for (const auto sample : {Case::NO_PREPARED, Case::ALREADY_DELIVERED, Case::OTHER_KEY,
                              Case::OTHER_DIGEST}) {
        TempJournal journal;
        auto authority = std::make_shared<DomainAuthority>();
        auto opened = open_store(journal, authority);
        start(*opened.store, 1);
        if (sample != Case::NO_PREPARED)
            RECOVERY_CHECK(!error_of(prepare(*opened.store, key(1, 1), digest_of(1))).has_value());
        if (sample == Case::ALREADY_DELIVERED)
            RECOVERY_CHECK(!error_of(opened.store->write_delivered(
                key(1, 1), digest_of(1), Access::unarmed_ticket())).has_value());
        const auto bytes_before = journal.bytes();
        const auto target = sample == Case::OTHER_KEY ? key(1, 2) : key(1, 1);
        const auto digest = sample == Case::OTHER_DIGEST ? digest_of(2) : digest_of(1);
        const auto result = opened.store->write_delivered(target, digest, Access::unarmed_ticket());
        RECOVERY_CHECK(error_of(result) == JournalError::CORRUPT);
        RECOVERY_CHECK_EQ(opened.store->wrapper_state(), JournalWrapperState::POISONED);
        RECOVERY_CHECK(journal.bytes() == bytes_before);   // nothing written
        // Every queued mutation after the poison returns POISONED without advancing.
        const auto advances = authority->advances;
        RECOVERY_CHECK(error_of(opened.store->write_delivered(key(1, 1), digest_of(1),
                                                              Access::unarmed_ticket()))
                       == JournalError::POISONED);
        RECOVERY_CHECK_EQ(authority->advances, advances);
    }
}

namespace {
// A scripted implementation whose results the wrapper must re-verify.
class ScriptedStore final : public recovery::FaultJournalStore {
public:
    enum class Lie { NONE, WRONG_GENERATION, OLD_GENERATION, WRONG_KEY, WRONG_STATE, FAIL };
    Lie lie{Lie::NONE};
    std::size_t impl_calls{0};
    explicit ScriptedStore(std::shared_ptr<recovery::StoreDomainAuthority> authority)
        : FaultJournalStore(std::move(authority), kDomain) {}
    std::variant<recovery::CurrentBootRecordReservation, JournalError>
    reserve_current_boot_record(const recovery::DedupKey& k) noexcept override {
        auto reservation = make_current_boot_reservation(k, 1);
        if (!reservation) return JournalError::CORRUPT;
        return std::move(*reservation);
    }
    void release_current_boot_reservation(std::uint64_t) noexcept override {}
private:
    recovery::JournalWriteResult answer(std::uint64_t generation, const recovery::DedupKey& k,
                                        recovery::Digest d, JournalRecordState state) noexcept {
        ++impl_calls;
        recovery::FaultJournalRecord record{k, d, state, generation, 0, 0};
        switch (lie) {
        case Lie::NONE: break;
        case Lie::WRONG_GENERATION: record.store_generation += 1; break;
        case Lie::OLD_GENERATION: record.store_generation -= 1; break;
        case Lie::WRONG_KEY: record.key.delivery.value[1] ^= 1U; break;
        case Lie::WRONG_STATE:
            record.state = state == JournalRecordState::PREPARED ? JournalRecordState::DELIVERED
                                                                  : JournalRecordState::PREPARED;
            break;
        case Lie::FAIL: return JournalError::IO_FAILED;
        }
        return record;
    }
    recovery::JournalWriteResult write_prepared_impl(std::uint64_t, std::uint64_t generation,
        const recovery::DedupKey& k, recovery::Digest d, const recovery::DeadlineTicket&) noexcept override {
        return answer(generation, k, d, JournalRecordState::PREPARED);
    }
    recovery::JournalWriteResult write_delivered_impl(std::uint64_t generation,
        const recovery::DedupKey& k, recovery::Digest d, const recovery::DeadlineTicket&) noexcept override {
        return answer(generation, k, d, JournalRecordState::DELIVERED);
    }
    std::variant<std::uint64_t, JournalError> read_observed_generation_impl() noexcept override {
        return std::uint64_t{0};
    }
    recovery::JournalStartupPlanResult scan_startup_impl(recovery::CoordinatorBootId,
                                                         recovery::RetentionEpochAuthority&) noexcept override {
        return recovery::JournalStartupPlan{};
    }
    std::variant<std::monostate, JournalError> reclaim_old_boot_delivered_impl(
        const recovery::FaultJournalRecord&, std::uint64_t) noexcept override {
        return std::monostate{};
    }
};
}  // namespace

// G21: the wrapper re-verifies every field the impl returns; a wrong or old
// generation, key or state — or an impl failure — leaves it POISONED, and
// every later mutation returns POISONED with zero further advances or impl calls.
RECOVERY_TEST(G21_wrapper_rejects_untrue_impl_results_and_stays_poisoned) {
    using Lie = ScriptedStore::Lie;
    for (const auto lie : {Lie::WRONG_GENERATION, Lie::OLD_GENERATION, Lie::WRONG_KEY,
                           Lie::WRONG_STATE, Lie::FAIL}) {
        for (const bool delivered : {false, true}) {
            auto authority = std::make_shared<DomainAuthority>();
            auto store = std::make_shared<ScriptedStore>(authority);
            start(*store, 1);
            if (delivered) RECOVERY_CHECK(!error_of(prepare(*store, key(1, 1), digest_of(1))).has_value());
            store->lie = lie;
            const auto result = delivered
                ? store->write_delivered(key(1, 1), digest_of(1), Access::unarmed_ticket())
                : prepare(*store, key(1, 1), digest_of(1));
            RECOVERY_CHECK(error_of(result).has_value());
            RECOVERY_CHECK_EQ(store->wrapper_state(), JournalWrapperState::POISONED);
            const auto advances = authority->advances;
            const auto calls = store->impl_calls;
            store->lie = Lie::NONE;
            RECOVERY_CHECK(error_of(prepare(*store, key(1, 2), digest_of(2))) == JournalError::POISONED);
            RECOVERY_CHECK(error_of(store->write_delivered(key(1, 1), digest_of(1),
                                                           Access::unarmed_ticket()))
                           == JournalError::POISONED);
            RECOVERY_CHECK_EQ(authority->advances, advances);
            RECOVERY_CHECK_EQ(store->impl_calls, calls);
        }
    }
    {   // a failed authority advance also poisons before the impl is reached
        auto authority = std::make_shared<DomainAuthority>();
        auto store = std::make_shared<ScriptedStore>(authority);
        start(*store, 1);
        authority->fail_advance = true;
        RECOVERY_CHECK(error_of(prepare(*store, key(1, 1), digest_of(1)))
                       == JournalError::AUTHORITY_UNAVAILABLE);
        RECOVERY_CHECK_EQ(store->impl_calls, 0U);
        RECOVERY_CHECK_EQ(store->wrapper_state(), JournalWrapperState::POISONED);
    }
}

// G10: WAL write failure, short (torn) write and fsync failure all poison the
// store; the restart then fails closed instead of trusting the journal.
RECOVERY_TEST(G10_wal_write_torn_write_and_fsync_failures_fail_closed) {
    for (int sample = 0; sample < 3; ++sample) {
        TempJournal journal;
        auto authority = std::make_shared<DomainAuthority>();
        auto ops = std::make_shared<FailingOps>();
        auto opened = open_store(journal, authority, 16, 10, ops);
        start(*opened.store, 1);
        ops->fail_write = sample == 0;
        ops->short_write = sample == 1;
        ops->fail_fsync = sample == 2;
        RECOVERY_CHECK(error_of(prepare(*opened.store, key(1, 1), digest_of(1))) == JournalError::IO_FAILED);
        RECOVERY_CHECK_EQ(opened.store->wrapper_state(), JournalWrapperState::POISONED);
        auto reopened = open_store(journal, authority);
        Epoch retention{100};
        const auto restarted = reopened.store->startup_scan_and_reclaim_old_boot({1}, retention);
        if (sample < 2) {
            // nothing or a torn tail reached the file: floor ahead, or torn record
            RECOVERY_CHECK(std::holds_alternative<JournalError>(restarted));
        } else {
            // fsync failed after the bytes reached the file: durability unknown. The
            // record may surface, but only as a same-boot PREPARED, which ends the
            // boot at ingress create() (G16①) — never as anything ACK-visible.
            if (const auto* snapshot = std::get_if<recovery::JournalStartupSnapshot>(&restarted)) {
                RECOVERY_CHECK_EQ(snapshot->current_boot_records.size(), 1U);
                RECOVERY_CHECK_EQ(snapshot->current_boot_records.front().state,
                                  JournalRecordState::PREPARED);
            }
        }
    }
}

// G10: capacity is explicit; a PREPARED record is never evicted to make room.
RECOVERY_TEST(G10_capacity_is_explicit_and_prepared_records_stay) {
    TempJournal journal;
    auto authority = std::make_shared<DomainAuthority>();
    {
        auto opened = open_store(journal, authority, 2);
        start(*opened.store, 1);
        RECOVERY_CHECK(!error_of(prepare(*opened.store, key(1, 1), digest_of(1))).has_value());
        auto second = opened.store->reserve_current_boot_record(key(1, 2));
        RECOVERY_CHECK(std::holds_alternative<recovery::CurrentBootRecordReservation>(second));
        const auto third = opened.store->reserve_current_boot_record(key(1, 3));
        RECOVERY_CHECK(std::holds_alternative<JournalError>(third));
        RECOVERY_CHECK(std::get<JournalError>(third) == JournalError::CAPACITY);
    }
    auto reopened = open_store(journal, authority, 2);
    const auto snapshot = start(*reopened.store, 1);
    RECOVERY_CHECK_EQ(snapshot.current_boot_records.size(), 1U);
    RECOVERY_CHECK_EQ(snapshot.current_boot_records.front().state, JournalRecordState::PREPARED);
}

// G10: four corrupt-record samples, each rejected at startup.
RECOVERY_TEST(G10_corrupt_records_are_rejected_at_startup) {
    auto authority_for = [](std::uint64_t floor) {
        auto authority = std::make_shared<DomainAuthority>();
        authority->floor = floor;
        return authority;
    };
    {   // checksum mismatch: one flipped payload byte breaks the chain
        TempJournal journal;
        auto authority = authority_for(0);
        { auto opened = open_store(journal, authority); start(*opened.store, 1);
          RECOVERY_CHECK(!error_of(prepare(*opened.store, key(1, 1), digest_of(1))).has_value()); }
        auto bytes = journal.bytes();
        bytes.back() ^= 0x01;
        journal.overwrite(bytes);
        auto reopened = open_store(journal, authority);
        RECOVERY_CHECK(startup_error(*reopened.store, 1) == JournalError::CORRUPT);
    }
    const auto raw_case = [&](const std::vector<std::uint8_t>& payload) {
        TempJournal journal;
        { auto writer = open_store(journal, authority_for(0));
          RECOVERY_CHECK(Access::append_raw_journal_entry(*writer.store, payload)); }
        auto reopened = open_store(journal, authority_for(1));
        return startup_error(*reopened.store, 2);
    };
    // missing field
    RECOVERY_CHECK(raw_case(raw_prepared(1, key(1, 1), true)) == JournalError::CORRUPT);
    // illegal state value
    RECOVERY_CHECK(raw_case({9, 1, 0, 0, 0, 0, 0, 0, 0}) == JournalError::CORRUPT);
    // illegal CoordinatorBootId: zero, and one from the future
    RECOVERY_CHECK(raw_case(raw_prepared(1, key(0, 1))) == JournalError::CORRUPT);
    RECOVERY_CHECK(raw_case(raw_prepared(1, key(3, 1))) == JournalError::CORRUPT);
    // control: the same well-formed entry is accepted
    RECOVERY_CHECK(!raw_case(raw_prepared(1, key(1, 1))).has_value());
}

// G10: a commit-lock failure poisons the store.
RECOVERY_TEST(G10_commit_lock_failure_poisons_the_store) {
    TempJournal journal;
    auto authority = std::make_shared<DomainAuthority>();
    auto opened = open_store(journal, authority);
    start(*opened.store, 1);
    Access::fail_next_journal_lock(*opened.store);
    RECOVERY_CHECK(error_of(prepare(*opened.store, key(1, 1), digest_of(1))) == JournalError::POISONED);
    RECOVERY_CHECK_EQ(opened.store->wrapper_state(), JournalWrapperState::POISONED);
    RECOVERY_CHECK(error_of(prepare(*opened.store, key(1, 2), digest_of(2))) == JournalError::POISONED);
    RECOVERY_CHECK_EQ(authority->advances, 0U);
}

// The external floor catches a rolled-back journal and a domain mismatch.
RECOVERY_TEST(G10_rolled_back_journal_and_wrong_domain_fail_closed) {
    TempJournal journal;
    auto authority = std::make_shared<DomainAuthority>();
    std::vector<char> old_bytes;
    {
        auto opened = open_store(journal, authority);
        start(*opened.store, 1);
        old_bytes = journal.bytes();
        RECOVERY_CHECK(!error_of(prepare(*opened.store, key(1, 1), digest_of(1))).has_value());
    }
    journal.overwrite(old_bytes);   // journal rolled back behind the floor
    auto rolled = open_store(journal, authority);
    RECOVERY_CHECK(startup_error(*rolled.store, 1) == JournalError::GENERATION_MISMATCH);
    auto other = std::make_shared<DomainAuthority>();
    other->expected = recovery::StoreDomainId{{0xee}};
    auto mismatched = open_store(journal, other);
    RECOVERY_CHECK(startup_error(*mismatched.store, 1) == JournalError::GENERATION_MISMATCH);
}

// G13 / G16② / G10 summary: boot 1 leaves PREPARED, DELIVERED and a second
// PREPARED; boot 2 sees none of them as current, reclaims only the DELIVERED,
// and the summary is reproducible from the journal alone.
RECOVERY_TEST(G13_G16_old_boot_records_are_audit_only_and_reclaim_leaves_a_tombstone) {
    TempJournal journal;
    auto authority = std::make_shared<DomainAuthority>();
    {
        auto opened = open_store(journal, authority, 16, 10, nullptr, 100);
        start(*opened.store, 1);
        RECOVERY_CHECK(!error_of(prepare(*opened.store, key(1, 1), digest_of(1))).has_value());
        deliver(*opened.store, key(1, 2), digest_of(2));
        RECOVERY_CHECK(!error_of(prepare(*opened.store, key(1, 3), digest_of(3))).has_value());
    }
    const auto before_reclaim = journal.bytes();
    const auto floor_before_reclaim = authority->floor;
    recovery::StaleAuditSummary without_reclaim{};
    {   // retention not yet satisfied: nothing reclaimed
        auto opened = open_store(journal, authority);
        const auto snapshot = start(*opened.store, 2, 105);
        RECOVERY_CHECK(snapshot.current_boot_records.empty());
        RECOVERY_CHECK_EQ(snapshot.stale_audit.stale_boot_count, 1U);
        RECOVERY_CHECK_EQ(snapshot.stale_audit.stale_prepared, 2U);
        RECOVERY_CHECK_EQ(snapshot.stale_audit.stale_delivered, 1U);
        RECOVERY_CHECK_EQ(snapshot.stale_audit.reclaimed, 0U);
        without_reclaim = snapshot.stale_audit;
        // boot 2 may journal the same delivery id: a different key, a new fact
        RECOVERY_CHECK(!error_of(prepare(*opened.store, key(2, 2), digest_of(2))).has_value());
    }
    journal.overwrite(before_reclaim);
    authority->floor = floor_before_reclaim;
    recovery::FaultJournalRecord reclaimed_record{};
    {   // retention satisfied: only the old DELIVERED goes, with a tombstone
        auto opened = open_store(journal, authority);
        const auto snapshot = start(*opened.store, 2, 200);
        RECOVERY_CHECK_EQ(snapshot.stale_audit.stale_prepared, 2U);
        RECOVERY_CHECK_EQ(snapshot.stale_audit.stale_delivered, 1U);
        RECOVERY_CHECK_EQ(snapshot.stale_audit.reclaimed, 1U);
        reclaimed_record = {key(1, 2), digest_of(2), JournalRecordState::DELIVERED, 3, 0, 100};
        RECOVERY_CHECK(snapshot.stale_audit.digest
                       == recovery::reclaim_tombstone_digest(without_reclaim.digest, reclaimed_record));
    }
    {   // after restart the tombstone is durable: the DELIVERED is gone, both PREPARED stay
        auto opened = open_store(journal, authority);
        const auto snapshot = start(*opened.store, 3, 300);
        RECOVERY_CHECK_EQ(snapshot.stale_audit.stale_prepared, 2U);
        RECOVERY_CHECK_EQ(snapshot.stale_audit.stale_delivered, 0U);
        RECOVERY_CHECK_EQ(snapshot.stale_audit.reclaimed, 0U);
    }
}

// G16①: a same-boot PREPARED with no live owner surfaces in the startup snapshot.
RECOVERY_TEST(G16_same_boot_prepared_surfaces_at_startup) {
    TempJournal journal;
    auto authority = std::make_shared<DomainAuthority>();
    { auto opened = open_store(journal, authority); start(*opened.store, 1);
      RECOVERY_CHECK(!error_of(prepare(*opened.store, key(1, 1), digest_of(1))).has_value()); }
    auto reopened = open_store(journal, authority);
    const auto snapshot = start(*reopened.store, 1);
    RECOVERY_CHECK_EQ(snapshot.current_boot_records.size(), 1U);
    RECOVERY_CHECK_EQ(snapshot.current_boot_records.front().state, JournalRecordState::PREPARED);
}

// Retention needs a comparable, non-regressed epoch; otherwise nothing is reclaimed.
RECOVERY_TEST(G13_retention_epoch_unavailable_or_regressed_reclaims_nothing) {
    TempJournal journal;
    auto authority = std::make_shared<DomainAuthority>();
    { auto opened = open_store(journal, authority, 16, 10, nullptr, 100); start(*opened.store, 1);
      deliver(*opened.store, key(1, 1), digest_of(1)); }
    const auto bytes = journal.bytes();
    const auto floor = authority->floor;
    {   // regressed epoch (older than a recorded one)
        auto opened = open_store(journal, authority);
        RECOVERY_CHECK_EQ(start(*opened.store, 2, 50).stale_audit.reclaimed, 0U);
    }
    journal.overwrite(bytes);
    authority->floor = floor;
    {   // unavailable epoch
        auto opened = open_store(journal, authority);
        Epoch missing{0};
        missing.value = JournalError::AUTHORITY_UNAVAILABLE;
        auto result = opened.store->startup_scan_and_reclaim_old_boot({2}, missing);
        RECOVERY_CHECK(std::holds_alternative<recovery::JournalStartupSnapshot>(result));
        RECOVERY_CHECK_EQ(std::get<recovery::JournalStartupSnapshot>(result).stale_audit.reclaimed, 0U);
    }
}
