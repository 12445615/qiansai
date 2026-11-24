#ifndef RECOVERY_JOURNAL_HPP
#define RECOVERY_JOURNAL_HPP

// Fault-ingress journal (§4.6.5, §14): a nonvirtual wrapper that binds every
// durable mutation to an external StoreDomainAuthority, and a file-backed
// append-only implementation with checksums, a hash chain and an explicit
// durable barrier. Target durability semantics remain §19 item 24.

#include "recovery/ingress.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <sys/types.h>

namespace recovery {

enum class JournalRecordState : std::uint8_t { PREPARED, DELIVERED };
enum class JournalError : std::uint8_t {
    IO_FAILED, DEADLINE, CORRUPT, GENERATION_MISMATCH, CAPACITY,
    AUTHORITY_UNAVAILABLE, RESERVATION_KEY_MISMATCH, POISONED
};
enum class JournalWrapperState : std::uint8_t { UNVERIFIED, READY, POISONED };

// Expected domain identity and the monotonic StoreGeneration floor live
// outside the journal, so a rolled-back or mis-mounted journal cannot vouch
// for itself.
class StoreDomainAuthority {
public:
    virtual ~StoreDomainAuthority() noexcept = default;
    virtual std::variant<std::uint64_t, JournalError> verify_startup(
        StoreDomainId, std::uint64_t observed_generation) noexcept = 0;
    virtual std::variant<std::uint64_t, JournalError> verify_and_advance(
        StoreDomainId, std::uint64_t current_generation) noexcept = 0;
};
class RetentionEpochAuthority {
public:
    virtual ~RetentionEpochAuthority() noexcept = default;
    virtual std::variant<std::uint64_t, JournalError> current_epoch() noexcept = 0;
};

struct FaultJournalRecord final {
    DedupKey key;
    Digest payload_digest;
    JournalRecordState state;
    std::uint64_t store_generation{};
    TrustedMonotonicTime created_mono{};
    std::uint64_t created_epoch{};
    auto operator<=>(const FaultJournalRecord&) const = default;
};
using JournalWriteResult = std::variant<FaultJournalRecord, JournalError>;
struct StaleAuditSummary final {
    std::uint64_t stale_boot_count{};
    std::uint64_t stale_prepared{};
    std::uint64_t stale_delivered{};
    std::uint64_t reclaimed{};
    Digest digest{};   // old-boot classification plus every durable reclaim tombstone
    auto operator<=>(const StaleAuditSummary&) const = default;
};
struct JournalStartupSnapshot final {
    std::vector<FaultJournalRecord> current_boot_records;
    StaleAuditSummary stale_audit;
};
using JournalStartupResult = std::variant<JournalStartupSnapshot, JournalError>;
struct JournalStartupPlan final {
    std::vector<FaultJournalRecord> current_boot_records;
    std::vector<FaultJournalRecord> old_boot_delivered_to_reclaim;
    StaleAuditSummary stale_audit;   // reclaimed starts at 0
};
using JournalStartupPlanResult = std::variant<JournalStartupPlan, JournalError>;

// The tombstone digest a reclaim folds into StaleAuditSummary::digest.
[[nodiscard]] Digest reclaim_tombstone_digest(const Digest& previous, const FaultJournalRecord&);

class FaultJournalStore;
class CurrentBootRecordReservation final {
    friend class FaultJournalStore;
    std::shared_ptr<FaultJournalStore> store_;
    DedupKey key_;
    std::uint64_t slot_{};
    bool consumed_{};
    CurrentBootRecordReservation(std::shared_ptr<FaultJournalStore>, const DedupKey&,
                                 std::uint64_t slot) noexcept;
public:
    CurrentBootRecordReservation() = delete;
    CurrentBootRecordReservation(const CurrentBootRecordReservation&) = delete;
    CurrentBootRecordReservation& operator=(const CurrentBootRecordReservation&) = delete;
    CurrentBootRecordReservation(CurrentBootRecordReservation&&) noexcept;
    ~CurrentBootRecordReservation() noexcept;   // unconsumed: returns capacity, no I/O
    [[nodiscard]] const DedupKey& key() const noexcept { return key_; }
};

class FaultJournalStore : public std::enable_shared_from_this<FaultJournalStore> {
public:
    virtual ~FaultJournalStore() noexcept = default;
    FaultJournalStore(const FaultJournalStore&) = delete;
    FaultJournalStore& operator=(const FaultJournalStore&) = delete;
    // Listener-invisible startup: verify, classify, reclaim old-boot DELIVERED only.
    JournalStartupResult startup_scan_and_reclaim_old_boot(
        CoordinatorBootId current, RetentionEpochAuthority&) noexcept;
    virtual std::variant<CurrentBootRecordReservation, JournalError>
        reserve_current_boot_record(const DedupKey&) noexcept = 0;
    JournalWriteResult write_prepared(CurrentBootRecordReservation&&, const DedupKey&, Digest,
                                      const DeadlineTicket&) noexcept;
    // Not an upsert: exactly one current-boot PREPARED with this key and digest.
    JournalWriteResult write_delivered(const DedupKey&, Digest, const DeadlineTicket&) noexcept;
    virtual void release_current_boot_reservation(std::uint64_t slot) noexcept = 0;
    [[nodiscard]] JournalWrapperState wrapper_state() const noexcept;
protected:
    FaultJournalStore(std::shared_ptr<StoreDomainAuthority>, StoreDomainId) noexcept;
    std::optional<CurrentBootRecordReservation> make_current_boot_reservation(
        const DedupKey& key, std::uint64_t slot) noexcept;
    virtual JournalWriteResult write_prepared_impl(std::uint64_t reserved_slot,
        std::uint64_t committed_generation, const DedupKey&, Digest,
        const DeadlineTicket&) noexcept = 0;
    virtual JournalWriteResult write_delivered_impl(std::uint64_t committed_generation,
        const DedupKey&, Digest, const DeadlineTicket&) noexcept = 0;
    virtual std::variant<std::uint64_t, JournalError> read_observed_generation_impl() noexcept = 0;
    virtual JournalStartupPlanResult scan_startup_impl(
        CoordinatorBootId, RetentionEpochAuthority&) noexcept = 0;
    virtual std::variant<std::monostate, JournalError> reclaim_old_boot_delivered_impl(
        const FaultJournalRecord&, std::uint64_t committed_generation) noexcept = 0;
private:
#ifdef RECOVERY_TESTING
    friend struct RecoveryTestAccess;
    bool fail_next_lock_{};
#endif
    [[nodiscard]] bool lock_commit_mutex(std::unique_lock<std::mutex>&) noexcept;
    void finish_startup_verification_locked(std::uint64_t verified_generation) noexcept;
    std::variant<std::uint64_t, JournalError> begin_mutation_locked() noexcept;
    void commit_mutation_locked(std::uint64_t durable_generation) noexcept;
    const std::shared_ptr<StoreDomainAuthority> domain_authority_;
    const StoreDomainId store_domain_id_;
    std::mutex journal_commit_mutex_;
    std::optional<std::uint64_t> verified_generation_;
    std::atomic<JournalWrapperState> wrapper_state_{JournalWrapperState::UNVERIFIED};
};

// write/fsync seam so WAL and durable-barrier failures can be injected.
class JournalFileOps {
public:
    virtual ~JournalFileOps() noexcept = default;
    virtual ssize_t write(int fd, const void* data, std::size_t size) noexcept;
    virtual int fsync(int fd) noexcept;
};

// Append-only journal file: [u32 length][32-byte chained digest][payload] per
// entry, digest = canonical_digest(previous digest, payload). Entry 0 is the
// header (domain + format); every later entry carries generation previous+1.
class FileFaultJournalStore final : public FaultJournalStore {
public:
    struct Options {
        std::string path;
        StoreDomainId domain;
        std::size_t capacity;              // live records + outstanding reservations
        std::uint64_t retention_epochs;    // old-boot DELIVERED older than this may be reclaimed
        std::shared_ptr<RetentionEpochAuthority> write_epochs;  // stamps created_epoch
        std::shared_ptr<JournalFileOps> ops;                     // null = real syscalls
    };
    static constexpr std::uint32_t kFormatVersion = 1;
    // Provisioning: writes the header and makes it durable. Not part of startup.
    static std::variant<std::monostate, JournalError> initialize(
        const std::string& path, StoreDomainId, JournalFileOps* ops = nullptr) noexcept;
    static std::shared_ptr<FileFaultJournalStore> open(
        Options, std::shared_ptr<StoreDomainAuthority>) noexcept;

    std::variant<CurrentBootRecordReservation, JournalError>
        reserve_current_boot_record(const DedupKey&) noexcept override;
    void release_current_boot_reservation(std::uint64_t slot) noexcept override;
private:
#ifdef RECOVERY_TESTING
    friend struct RecoveryTestAccess;
#endif
    FileFaultJournalStore(Options, std::shared_ptr<StoreDomainAuthority>) noexcept;
    JournalWriteResult write_prepared_impl(std::uint64_t, std::uint64_t, const DedupKey&, Digest,
                                           const DeadlineTicket&) noexcept override;
    JournalWriteResult write_delivered_impl(std::uint64_t, const DedupKey&, Digest,
                                            const DeadlineTicket&) noexcept override;
    std::variant<std::uint64_t, JournalError> read_observed_generation_impl() noexcept override;
    JournalStartupPlanResult scan_startup_impl(CoordinatorBootId,
                                               RetentionEpochAuthority&) noexcept override;
    std::variant<std::monostate, JournalError> reclaim_old_boot_delivered_impl(
        const FaultJournalRecord&, std::uint64_t) noexcept override;

    struct Loaded {
        std::map<DedupKey, FaultJournalRecord> records;
        std::uint64_t generation{};
        Digest tail{};
    };
    std::variant<Loaded, JournalError> load() noexcept;
    std::variant<std::monostate, JournalError> append(const std::vector<std::uint8_t>& payload) noexcept;

    Options options_;
    std::shared_ptr<JournalFileOps> ops_;
    std::mutex state_mutex_;                       // guards the fields below; never nested outward
    std::map<DedupKey, FaultJournalRecord> records_;
    std::map<std::uint64_t, DedupKey> reservations_;
    std::uint64_t next_slot_{1};
    std::optional<CoordinatorBootId> current_boot_;
    Digest tail_{};
    bool loaded_{};
};

}  // namespace recovery
#endif
