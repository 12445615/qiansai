#include "recovery/journal.hpp"

#include "recovery/canonical.hpp"
#include "recovery/canonical_reader.hpp"
#include "recovery/crypto.hpp"

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <set>
#include <string>
#include <system_error>
#include <tuple>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace recovery {
namespace {
enum class EntryKind : std::uint8_t { HEADER = 0, PREPARED = 1, DELIVERED = 2, TOMBSTONE = 3 };
constexpr std::size_t kFrameHeaderBytes = 4 + 32;
constexpr std::uint32_t kMaxPayloadBytes = 4096;

Digest chain_digest(const Digest& previous, const std::vector<std::uint8_t>& payload) {
    return canonical_digest(std::pair<Digest, std::vector<std::uint8_t>>{previous, payload});
}

Digest key_hash(const DedupKey& key) { return canonical_digest(key); }

TrustedMonotonicTime monotonic_now() noexcept {
    timespec now{};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<TrustedMonotonicTime>(now.tv_sec) * 1'000'000'000ULL
        + static_cast<TrustedMonotonicTime>(now.tv_nsec);
}

bool read_key(CanonicalReader& reader, DedupKey& key) {
    return reader.read(key.coordinator_boot.value)
        && reader.read_raw(key.source.linux_boot.bytes.data(), key.source.linux_boot.bytes.size())
        && reader.read(key.source.registration.value)
        && reader.read(key.source.process.pid) && reader.read(key.source.process.start_generation)
        && reader.read(key.delivery.value);
}

std::vector<std::uint8_t> header_payload(const StoreDomainId& domain) {
    CanonicalWriter writer;
    canonical_write(writer, static_cast<std::uint8_t>(EntryKind::HEADER));
    canonical_write(writer, std::uint64_t{0});
    canonical_write(writer, domain.value);
    canonical_write(writer, FileFaultJournalStore::kFormatVersion);
    return std::move(writer).finish();
}

std::vector<std::uint8_t> frame(const Digest& previous, const std::vector<std::uint8_t>& payload,
                                Digest& digest_out) {
    digest_out = chain_digest(previous, payload);
    CanonicalWriter writer;
    canonical_write(writer, static_cast<std::uint32_t>(payload.size()));
    writer.write_bytes(digest_out.data(), digest_out.size());
    writer.write_bytes(payload.data(), payload.size());
    return std::move(writer).finish();
}

// One write; a short or failed write is a WAL failure, never retried here.
bool write_all(JournalFileOps& ops, int fd, const std::vector<std::uint8_t>& bytes) noexcept {
    const ssize_t written = ops.write(fd, bytes.data(), bytes.size());
    return written >= 0 && static_cast<std::size_t>(written) == bytes.size();
}

bool fsync_directory_of(JournalFileOps& ops, const std::string& path) noexcept {
    const auto slash = path.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const OwnedFd owned{fd};
    return ops.fsync(owned.get()) == 0;
}

JournalFileOps& default_ops() {
    static JournalFileOps ops;
    return ops;
}
}  // namespace

ssize_t JournalFileOps::write(int fd, const void* data, std::size_t size) noexcept {
    ssize_t result = -1;
    do { result = ::write(fd, data, size); } while (result < 0 && errno == EINTR);
    return result;
}

int JournalFileOps::fsync(int fd) noexcept { return ::fsync(fd); }

Digest reclaim_tombstone_digest(const Digest& previous, const FaultJournalRecord& record) {
    return canonical_digest(std::pair<Digest, std::pair<std::uint64_t, Digest>>{
        previous, {record.key.coordinator_boot.value, key_hash(record.key)}});
}

// ===== CurrentBootRecordReservation =====
CurrentBootRecordReservation::CurrentBootRecordReservation(
    std::shared_ptr<FaultJournalStore> store, const DedupKey& key, std::uint64_t slot) noexcept
    : store_(std::move(store)), key_(key), slot_(slot) {}

CurrentBootRecordReservation::CurrentBootRecordReservation(
    CurrentBootRecordReservation&& other) noexcept
    : store_(std::move(other.store_)), key_(other.key_), slot_(other.slot_),
      consumed_(std::exchange(other.consumed_, true)) {}

CurrentBootRecordReservation::~CurrentBootRecordReservation() noexcept {
    if (!consumed_ && store_) store_->release_current_boot_reservation(slot_);
}

// ===== wrapper =====
FaultJournalStore::FaultJournalStore(std::shared_ptr<StoreDomainAuthority> authority,
                                     StoreDomainId domain) noexcept
    : domain_authority_(std::move(authority)), store_domain_id_(domain) {}

JournalWrapperState FaultJournalStore::wrapper_state() const noexcept {
    return wrapper_state_.load(std::memory_order_acquire);
}

std::optional<CurrentBootRecordReservation> FaultJournalStore::make_current_boot_reservation(
    const DedupKey& key, std::uint64_t slot) noexcept {
    auto self = weak_from_this().lock();
    if (!self) return std::nullopt;
    return CurrentBootRecordReservation{std::move(self), key, slot};
}

// A commit-lock failure poisons the store for good: nothing behind it can be trusted.
bool FaultJournalStore::lock_commit_mutex(std::unique_lock<std::mutex>& guard) noexcept {
    try {
#ifdef RECOVERY_TESTING
        if (std::exchange(fail_next_lock_, false))
            throw std::system_error{std::make_error_code(std::errc::resource_deadlock_would_occur)};
#endif
        guard = std::unique_lock<std::mutex>{journal_commit_mutex_};
        return true;
    } catch (...) {
        wrapper_state_.store(JournalWrapperState::POISONED, std::memory_order_release);
        return false;
    }
}

void FaultJournalStore::finish_startup_verification_locked(std::uint64_t verified) noexcept {
    verified_generation_ = verified;
    wrapper_state_.store(JournalWrapperState::READY, std::memory_order_release);
}

// READY → authority advance → POISONED before the impl runs; any failure,
// including a failed advance, leaves the store POISONED permanently.
std::variant<std::uint64_t, JournalError> FaultJournalStore::begin_mutation_locked() noexcept {
    if (wrapper_state_.load(std::memory_order_acquire) != JournalWrapperState::READY
        || !verified_generation_ || !domain_authority_)
        return JournalError::POISONED;
    const auto advanced = domain_authority_->verify_and_advance(store_domain_id_, *verified_generation_);
    wrapper_state_.store(JournalWrapperState::POISONED, std::memory_order_release);
    if (const auto* error = std::get_if<JournalError>(&advanced)) return *error;
    const auto next = std::get<std::uint64_t>(advanced);
    if (next != *verified_generation_ + 1) return JournalError::GENERATION_MISMATCH;
    return next;
}

void FaultJournalStore::commit_mutation_locked(std::uint64_t durable_generation) noexcept {
    verified_generation_ = durable_generation;
    wrapper_state_.store(JournalWrapperState::READY, std::memory_order_release);
}

JournalStartupResult FaultJournalStore::startup_scan_and_reclaim_old_boot(
    CoordinatorBootId current, RetentionEpochAuthority& retention) noexcept {
    std::unique_lock<std::mutex> guard;
    if (!lock_commit_mutex(guard)) return JournalError::POISONED;
    if (wrapper_state_.load(std::memory_order_acquire) != JournalWrapperState::UNVERIFIED
        || !domain_authority_)
        return JournalError::POISONED;
    const auto poison = [this](JournalError error) -> JournalStartupResult {
        wrapper_state_.store(JournalWrapperState::POISONED, std::memory_order_release);
        return error;
    };
    const auto observed = read_observed_generation_impl();
    if (const auto* error = std::get_if<JournalError>(&observed)) return poison(*error);
    const auto verified =
        domain_authority_->verify_startup(store_domain_id_, std::get<std::uint64_t>(observed));
    if (const auto* error = std::get_if<JournalError>(&verified)) return poison(*error);
    if (std::get<std::uint64_t>(verified) != std::get<std::uint64_t>(observed))
        return poison(JournalError::GENERATION_MISMATCH);
    auto planned = scan_startup_impl(current, retention);
    if (const auto* error = std::get_if<JournalError>(&planned)) return poison(*error);
    auto& plan = std::get<JournalStartupPlan>(planned);
    finish_startup_verification_locked(std::get<std::uint64_t>(verified));
    try {
        StaleAuditSummary summary = plan.stale_audit;
        for (const auto& record : plan.old_boot_delivered_to_reclaim) {
            const auto generation = begin_mutation_locked();
            if (const auto* error = std::get_if<JournalError>(&generation)) return *error;
            const auto reclaimed =
                reclaim_old_boot_delivered_impl(record, std::get<std::uint64_t>(generation));
            if (const auto* error = std::get_if<JournalError>(&reclaimed)) return *error;
            commit_mutation_locked(std::get<std::uint64_t>(generation));
            summary.reclaimed += 1;
            summary.digest = reclaim_tombstone_digest(summary.digest, record);
        }
        return JournalStartupSnapshot{std::move(plan.current_boot_records), summary};
    } catch (...) {
        return poison(JournalError::IO_FAILED);
    }
}

JournalWriteResult FaultJournalStore::write_prepared(CurrentBootRecordReservation&& reservation,
                                                     const DedupKey& key, Digest digest,
                                                     const DeadlineTicket& ticket) noexcept {
    std::unique_lock<std::mutex> guard;
    if (!lock_commit_mutex(guard)) return JournalError::POISONED;
    // Rejected before any advance: the store keeps serving well-formed callers.
    if (reservation.store_.get() != this || reservation.consumed_ || !(reservation.key_ == key))
        return JournalError::RESERVATION_KEY_MISMATCH;
    const auto generation = begin_mutation_locked();
    if (const auto* error = std::get_if<JournalError>(&generation)) return *error;
    reservation.consumed_ = true;
    const auto committed = std::get<std::uint64_t>(generation);
    const auto written = write_prepared_impl(reservation.slot_, committed, key, digest, ticket);
    if (const auto* error = std::get_if<JournalError>(&written)) return *error;
    const auto& record = std::get<FaultJournalRecord>(written);
    if (!(record.key == key) || record.payload_digest != digest
        || record.state != JournalRecordState::PREPARED || record.store_generation != committed)
        return JournalError::CORRUPT;
    commit_mutation_locked(committed);
    return record;
}

JournalWriteResult FaultJournalStore::write_delivered(const DedupKey& key, Digest digest,
                                                      const DeadlineTicket& ticket) noexcept {
    std::unique_lock<std::mutex> guard;
    if (!lock_commit_mutex(guard)) return JournalError::POISONED;
    const auto generation = begin_mutation_locked();
    if (const auto* error = std::get_if<JournalError>(&generation)) return *error;
    const auto committed = std::get<std::uint64_t>(generation);
    const auto written = write_delivered_impl(committed, key, digest, ticket);
    if (const auto* error = std::get_if<JournalError>(&written)) return *error;
    const auto& record = std::get<FaultJournalRecord>(written);
    if (!(record.key == key) || record.payload_digest != digest
        || record.state != JournalRecordState::DELIVERED || record.store_generation != committed)
        return JournalError::CORRUPT;
    commit_mutation_locked(committed);
    return record;
}

// ===== file implementation =====
std::variant<std::monostate, JournalError> FileFaultJournalStore::initialize(
    const std::string& path, StoreDomainId domain, JournalFileOps* ops) noexcept {
    try {
        JournalFileOps& io = ops ? *ops : default_ops();
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) return JournalError::IO_FAILED;
        const OwnedFd owned{fd};
        Digest digest{};
        if (!write_all(io, owned.get(), frame(Digest{}, header_payload(domain), digest))
            || io.fsync(owned.get()) != 0 || !fsync_directory_of(io, path))
            return JournalError::IO_FAILED;
        return std::monostate{};
    } catch (...) {
        return JournalError::IO_FAILED;
    }
}

std::shared_ptr<FileFaultJournalStore> FileFaultJournalStore::open(
    Options options, std::shared_ptr<StoreDomainAuthority> authority) noexcept {
    try {
        return std::shared_ptr<FileFaultJournalStore>(
            new FileFaultJournalStore(std::move(options), std::move(authority)));
    } catch (...) {
        return nullptr;
    }
}

FileFaultJournalStore::FileFaultJournalStore(Options options,
                                             std::shared_ptr<StoreDomainAuthority> authority) noexcept
    : FaultJournalStore(std::move(authority), options.domain), options_(std::move(options)),
      ops_(options_.ops) {}

std::variant<FileFaultJournalStore::Loaded, JournalError> FileFaultJournalStore::load() noexcept {
    try {
        const int fd = ::open(options_.path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return JournalError::IO_FAILED;
        const OwnedFd owned{fd};
        std::vector<std::uint8_t> bytes;
        std::uint8_t chunk[4096];
        for (;;) {
            const ssize_t n = ::read(owned.get(), chunk, sizeof chunk);
            if (n < 0) {
                if (errno == EINTR) continue;
                return JournalError::IO_FAILED;
            }
            if (n == 0) break;
            bytes.insert(bytes.end(), chunk, chunk + n);
        }
        Loaded loaded;
        std::size_t offset = 0;
        bool header_seen = false;
        while (offset < bytes.size()) {
            if (bytes.size() - offset < kFrameHeaderBytes) return JournalError::CORRUPT;  // torn
            CanonicalReader head{std::span<const std::uint8_t>{bytes.data() + offset, kFrameHeaderBytes}};
            std::uint32_t length{};
            Digest stored{};
            if (!head.read(length) || !head.read_raw(stored.data(), stored.size()))
                return JournalError::CORRUPT;
            offset += kFrameHeaderBytes;
            if (length > kMaxPayloadBytes || bytes.size() - offset < length)
                return JournalError::CORRUPT;  // torn tail or absurd length
            const std::vector<std::uint8_t> payload(
                bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
            offset += length;
            if (chain_digest(loaded.tail, payload) != stored) return JournalError::CORRUPT;
            loaded.tail = stored;

            CanonicalReader reader{payload};
            std::uint8_t kind{};
            std::uint64_t generation{};
            if (!reader.read(kind) || !reader.read(generation)) return JournalError::CORRUPT;
            if (!header_seen) {
                Digest domain{};
                std::uint32_t format{};
                if (kind != static_cast<std::uint8_t>(EntryKind::HEADER) || generation != 0
                    || !reader.read(domain) || !reader.read(format) || !reader.exhausted()
                    || domain != options_.domain.value || format != kFormatVersion)
                    return JournalError::CORRUPT;
                header_seen = true;
                continue;
            }
            if (generation != loaded.generation + 1) return JournalError::CORRUPT;  // gap or repeat
            loaded.generation = generation;
            switch (static_cast<EntryKind>(kind)) {
            case EntryKind::PREPARED: {
                FaultJournalRecord record{};
                if (!read_key(reader, record.key) || !reader.read(record.payload_digest)
                    || !reader.read(record.created_mono) || !reader.read(record.created_epoch)
                    || !reader.exhausted() || record.key.coordinator_boot.value == 0
                    || loaded.records.contains(record.key))
                    return JournalError::CORRUPT;
                record.state = JournalRecordState::PREPARED;
                record.store_generation = generation;
                loaded.records.emplace(record.key, record);
                break;
            }
            case EntryKind::DELIVERED: {
                DedupKey key{};
                Digest digest{};
                if (!read_key(reader, key) || !reader.read(digest) || !reader.exhausted())
                    return JournalError::CORRUPT;
                const auto found = loaded.records.find(key);
                if (found == loaded.records.end()
                    || found->second.state != JournalRecordState::PREPARED
                    || found->second.payload_digest != digest)
                    return JournalError::CORRUPT;
                found->second.state = JournalRecordState::DELIVERED;
                found->second.store_generation = generation;
                break;
            }
            case EntryKind::TOMBSTONE: {
                std::uint64_t boot{};
                std::uint8_t state{};
                Digest hash{};
                if (!reader.read(boot) || !reader.read(state) || !reader.read(hash)
                    || !reader.exhausted()
                    || state != static_cast<std::uint8_t>(JournalRecordState::DELIVERED))
                    return JournalError::CORRUPT;
                const auto found = std::find_if(loaded.records.begin(), loaded.records.end(),
                    [&](const auto& entry) { return key_hash(entry.first) == hash; });
                if (found == loaded.records.end()
                    || found->second.state != JournalRecordState::DELIVERED
                    || found->first.coordinator_boot.value != boot)
                    return JournalError::CORRUPT;
                loaded.records.erase(found);
                break;
            }
            default:
                return JournalError::CORRUPT;  // illegal state value
            }
        }
        if (!header_seen) return JournalError::CORRUPT;
        return loaded;
    } catch (...) {
        return JournalError::IO_FAILED;
    }
}

std::variant<std::uint64_t, JournalError> FileFaultJournalStore::read_observed_generation_impl() noexcept {
    auto loaded = load();
    if (const auto* error = std::get_if<JournalError>(&loaded)) return *error;
    auto& value = std::get<Loaded>(loaded);
    std::scoped_lock guard{state_mutex_};
    records_ = std::move(value.records);
    tail_ = value.tail;
    loaded_ = true;
    return value.generation;
}

JournalStartupPlanResult FileFaultJournalStore::scan_startup_impl(
    CoordinatorBootId current, RetentionEpochAuthority& retention) noexcept {
    try {
        std::scoped_lock guard{state_mutex_};
        if (!loaded_ || current.value == 0) return JournalError::CORRUPT;
        JournalStartupPlan plan;
        std::set<std::uint64_t> old_boots;
        std::vector<std::tuple<std::uint64_t, std::uint8_t, Digest>> classification;
        std::uint64_t newest_epoch = 0;
        for (const auto& [key, record] : records_) {
            if (key.coordinator_boot.value > current.value) return JournalError::CORRUPT;
            newest_epoch = std::max(newest_epoch, record.created_epoch);
            if (key.coordinator_boot == current) {
                plan.current_boot_records.push_back(record);
                continue;
            }
            old_boots.insert(key.coordinator_boot.value);
            if (record.state == JournalRecordState::PREPARED) plan.stale_audit.stale_prepared += 1;
            else plan.stale_audit.stale_delivered += 1;
            classification.emplace_back(key.coordinator_boot.value,
                                        static_cast<std::uint8_t>(record.state), key_hash(key));
        }
        plan.stale_audit.stale_boot_count = old_boots.size();
        std::sort(classification.begin(), classification.end());
        std::vector<std::pair<std::pair<std::uint64_t, std::uint8_t>, Digest>> canonical;
        canonical.reserve(classification.size());
        for (const auto& [boot, state, hash] : classification) canonical.push_back({{boot, state}, hash});
        plan.stale_audit.digest = canonical_digest(canonical);

        // Reclaim only with a comparable, non-regressed epoch; otherwise keep everything.
        const auto epoch = retention.current_epoch();
        if (const auto* now = std::get_if<std::uint64_t>(&epoch); now && *now >= newest_epoch) {
            for (const auto& [key, record] : records_) {
                if (key.coordinator_boot == current || record.state != JournalRecordState::DELIVERED)
                    continue;
                if (record.created_epoch <= *now && *now - record.created_epoch >= options_.retention_epochs)
                    plan.old_boot_delivered_to_reclaim.push_back(record);
            }
        }
        current_boot_ = current;
        return plan;
    } catch (...) {
        return JournalError::IO_FAILED;
    }
}

std::variant<std::monostate, JournalError> FileFaultJournalStore::append(
    const std::vector<std::uint8_t>& payload) noexcept {
    try {
        JournalFileOps& io = ops_ ? *ops_ : default_ops();
        Digest previous{};
        {
            std::scoped_lock guard{state_mutex_};
            previous = tail_;
        }
        Digest digest{};
        const auto bytes = frame(previous, payload, digest);
        const int fd = ::open(options_.path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
        if (fd < 0) return JournalError::IO_FAILED;
        const OwnedFd owned{fd};
        if (!write_all(io, owned.get(), bytes)) return JournalError::IO_FAILED;
        if (io.fsync(owned.get()) != 0) return JournalError::IO_FAILED;   // durable barrier
        std::scoped_lock guard{state_mutex_};
        tail_ = digest;
        return std::monostate{};
    } catch (...) {
        return JournalError::IO_FAILED;
    }
}

std::variant<CurrentBootRecordReservation, JournalError>
FileFaultJournalStore::reserve_current_boot_record(const DedupKey& key) noexcept {
    std::uint64_t slot{};
    try {
        std::scoped_lock guard{state_mutex_};
        if (!loaded_ || !current_boot_ || !(key.coordinator_boot == *current_boot_)
            || records_.contains(key))
            return JournalError::CORRUPT;
        for (const auto& [_, reserved] : reservations_)
            if (reserved == key) return JournalError::CORRUPT;
        if (records_.size() + reservations_.size() >= options_.capacity) return JournalError::CAPACITY;
        slot = next_slot_++;
        reservations_.emplace(slot, key);
    } catch (...) {
        return JournalError::CAPACITY;
    }
    auto reservation = make_current_boot_reservation(key, slot);
    if (!reservation) {
        release_current_boot_reservation(slot);
        return JournalError::CORRUPT;
    }
    return std::move(*reservation);
}

void FileFaultJournalStore::release_current_boot_reservation(std::uint64_t slot) noexcept {
    std::scoped_lock guard{state_mutex_};
    reservations_.erase(slot);
}

JournalWriteResult FileFaultJournalStore::write_prepared_impl(
    std::uint64_t slot, std::uint64_t generation, const DedupKey& key, Digest digest,
    const DeadlineTicket&) noexcept {
    try {
        {
            std::scoped_lock guard{state_mutex_};
            const auto found = reservations_.find(slot);
            if (found == reservations_.end() || !(found->second == key) || records_.contains(key))
                return JournalError::RESERVATION_KEY_MISMATCH;
        }
        if (!options_.write_epochs) return JournalError::AUTHORITY_UNAVAILABLE;
        const auto epoch = options_.write_epochs->current_epoch();
        if (const auto* error = std::get_if<JournalError>(&epoch)) return *error;
        FaultJournalRecord record{key, digest, JournalRecordState::PREPARED, generation,
                                  monotonic_now(), std::get<std::uint64_t>(epoch)};
        CanonicalWriter writer;
        canonical_write(writer, static_cast<std::uint8_t>(EntryKind::PREPARED));
        canonical_write(writer, generation);
        canonical_write(writer, key);
        canonical_write(writer, digest);
        canonical_write(writer, record.created_mono);
        canonical_write(writer, record.created_epoch);
        const auto appended = append(std::move(writer).finish());
        std::scoped_lock guard{state_mutex_};
        reservations_.erase(slot);
        if (const auto* error = std::get_if<JournalError>(&appended)) return *error;
        records_.emplace(key, record);
        return record;
    } catch (...) {
        return JournalError::IO_FAILED;
    }
}

JournalWriteResult FileFaultJournalStore::write_delivered_impl(
    std::uint64_t generation, const DedupKey& key, Digest digest, const DeadlineTicket&) noexcept {
    try {
        {
            std::scoped_lock guard{state_mutex_};
            const auto found = records_.find(key);
            if (!current_boot_ || found == records_.end() || !(key.coordinator_boot == *current_boot_)
                || found->second.state != JournalRecordState::PREPARED
                || found->second.payload_digest != digest)
                return JournalError::CORRUPT;   // never an upsert
        }
        CanonicalWriter writer;
        canonical_write(writer, static_cast<std::uint8_t>(EntryKind::DELIVERED));
        canonical_write(writer, generation);
        canonical_write(writer, key);
        canonical_write(writer, digest);
        const auto appended = append(std::move(writer).finish());
        if (const auto* error = std::get_if<JournalError>(&appended)) return *error;
        std::scoped_lock guard{state_mutex_};
        auto& record = records_.at(key);
        record.state = JournalRecordState::DELIVERED;
        record.store_generation = generation;
        return record;
    } catch (...) {
        return JournalError::IO_FAILED;
    }
}

std::variant<std::monostate, JournalError> FileFaultJournalStore::reclaim_old_boot_delivered_impl(
    const FaultJournalRecord& record, std::uint64_t generation) noexcept {
    try {
        {
            std::scoped_lock guard{state_mutex_};
            const auto found = records_.find(record.key);
            if (found == records_.end() || found->second.state != JournalRecordState::DELIVERED
                || (current_boot_ && record.key.coordinator_boot == *current_boot_))
                return JournalError::CORRUPT;
        }
        CanonicalWriter writer;
        canonical_write(writer, static_cast<std::uint8_t>(EntryKind::TOMBSTONE));
        canonical_write(writer, generation);
        canonical_write(writer, record.key.coordinator_boot.value);
        canonical_write(writer, static_cast<std::uint8_t>(JournalRecordState::DELIVERED));
        canonical_write(writer, key_hash(record.key));
        const auto appended = append(std::move(writer).finish());
        if (const auto* error = std::get_if<JournalError>(&appended)) return *error;
        std::scoped_lock guard{state_mutex_};
        records_.erase(record.key);
        return std::monostate{};
    } catch (...) {
        return JournalError::IO_FAILED;
    }
}

}  // namespace recovery
