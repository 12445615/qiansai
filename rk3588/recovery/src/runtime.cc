#include "recovery/runtime.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/syscall.h>
#if !defined(SYS_pidfd_open)
#if defined(__NR_pidfd_open)
#define SYS_pidfd_open __NR_pidfd_open
#elif defined(__aarch64__)
// pidfd_open is syscall 434 in the generic 64-bit Linux UAPI.  Older Jammy
// cross headers may omit the symbolic name even though the deployed kernel
// implements the stable syscall ABI.
#define SYS_pidfd_open 434
#else
#error "SYS_pidfd_open is unavailable for this target"
#endif
#endif
#if __has_include(<sys/pidfd.h>)
#include <sys/pidfd.h>
#define RECOVERY_PIDFD_NONBLOCK PIDFD_NONBLOCK
#else
#define RECOVERY_PIDFD_NONBLOCK O_NONBLOCK
#endif
#include <type_traits>
#include <unistd.h>

namespace recovery {
namespace {
class LinuxProcessExitKernel final : public ProcessExitKernel {
public:
    std::optional<std::uint64_t> read_start_generation(ProcessId pid) override {
        std::ifstream stat{"/proc/" + std::to_string(pid) + "/stat"};
        std::string line;
        if (!std::getline(stat, line)) return std::nullopt;
        const auto close = line.rfind(')');
        if (close == std::string::npos) return std::nullopt;
        std::istringstream fields{line.substr(close + 2)};
        std::string field;
        for (unsigned index = 3; index <= 22; ++index) {
            if (!(fields >> field)) return std::nullopt;
            if (index == 22) {
                try { return std::stoull(field); } catch (...) { return std::nullopt; }
            }
        }
        return std::nullopt;
    }
    OwnedFd pidfd_open(ProcessId pid) override {
        constexpr unsigned int kPidfdNonblock = RECOVERY_PIDFD_NONBLOCK;
        // Some deployed sysroots provide <sys/pidfd.h> but their libc does not
        // export pidfd_open.  Use the stable kernel ABI on every platform so
        // host and target builds have identical PIDFD_NONBLOCK semantics.
        return OwnedFd{static_cast<int>(
            ::syscall(SYS_pidfd_open, static_cast<pid_t>(pid), kPidfdNonblock))};
    }
};
LinuxProcessExitKernel linux_kernel;

Digest snapshot_digest(const FrozenRuntimeSnapshot& snapshot) {
    FrozenRuntimeSnapshot candidate = snapshot;
    candidate.snapshot_digest = {};
    return canonical_digest(candidate);
}

Digest owner_ack_view_digest(const OwnerSnapshotAck& ack) {
    OwnerSnapshotAck candidate = ack;
    candidate.local_view_digest = {};
    return canonical_digest(candidate);
}

bool snapshot_contents_match(const RecoveryInstanceRuntime& runtime) {
    return runtime.current_policy.contents && runtime.current_registry.contents
        && canonical_digest(*runtime.current_policy.contents) == runtime.current_policy.digest
        && canonical_digest(*runtime.current_registry.contents) == runtime.current_registry.digest;
}

bool expected_executors_match(const ValidatedRecoveryProgram& program,
                              const FrozenRuntimeSnapshot& snapshot,
                              const PolicySnapshot& policy) {
    const auto derived = canonical_action_closure(program.resource_plans(), snapshot, policy);
    const auto* dag = std::get_if<std::vector<ActionNode>>(&derived);
    if (!dag || dag->size() != program.action_dag().size()) return false;
    for (std::size_t i = 0; i < dag->size(); ++i)
        if ((*dag)[i].expected_executor != program.action_dag()[i].expected_executor) return false;
    return true;
}
}

OwnedFd::~OwnedFd() noexcept { if (fd_ >= 0) ::close(fd_); }
OwnedFd::OwnedFd(OwnedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
OwnedFd& OwnedFd::operator=(OwnedFd&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

RecoveryInstanceRuntime::RecoveryInstanceRuntime(RecoveryInstanceId id, FaultFact first)
    : recovery_id(id), fault_batch{std::move(first)} {}

FallbackKind worst_fallback(FallbackKind a, FallbackKind b) noexcept {
    return (a == FallbackKind::BOARD_RESTART_REQUIRED
            || b == FallbackKind::BOARD_RESTART_REQUIRED)
        ? FallbackKind::BOARD_RESTART_REQUIRED : FallbackKind::PARENT_SERVICE_RESTART;
}

bool descriptors_registry_agree(const std::vector<ProcessExitBindingDescriptor>& descriptors,
                                const ProcessExitBindingRegistry& registry) noexcept {
    if (descriptors.size() != registry.size()) return false;
    for (const auto& descriptor : descriptors) {
        const auto found = registry.find(descriptor.id);
        if (found == registry.end() || found->first != found->second.descriptor.id
            || found->second.descriptor != descriptor || !found->second.pidfd.valid()) return false;
    }
    return true;
}

RecoveryCoordinator::RecoveryCoordinator(BootId boot, ProcessExitKernel* kernel)
    : kernel_(kernel ? kernel : &linux_kernel) { slot_.boot_id = boot; }

#ifdef RECOVERY_TESTING
RecoveryCoordinator::RecoveryCoordinator(BootId boot, ProcessExitKernel* kernel,
                                         RuntimeFailureProvider* provider)
    : RecoveryCoordinator(boot, kernel) { failure_provider_ = provider; }

void RecoveryCoordinator::checkpoint(RuntimeFailureStage stage) {
    if (failure_provider_) failure_provider_->checkpoint(stage);
}
#endif

void fatal_watchdog_reboot(const char* reason) noexcept {
    static constexpr char prefix[] = "recovery: fatal watchdog reboot: ";
    (void)!::write(STDERR_FILENO, prefix, sizeof prefix - 1);
    if (reason) (void)!::write(STDERR_FILENO, reason, std::strlen(reason));
    (void)!::write(STDERR_FILENO, "\n", 1);
    std::_Exit(kWatchdogRebootExitCode);
}

#ifdef RECOVERY_TESTING
std::unique_lock<std::mutex> RecoveryCoordinator::lock_or_watchdog(
    RuntimeFailureStage stage, const char* reason) noexcept {
    try {
        checkpoint(stage);
        return std::unique_lock<std::mutex>{mutex_};
    } catch (...) {
        fatal_watchdog_reboot(reason);
    }
}
#define RECOVERY_LOCK(stage, reason) lock_or_watchdog(RuntimeFailureStage::stage, reason)
#else
std::unique_lock<std::mutex> RecoveryCoordinator::lock_or_watchdog(const char* reason) noexcept {
    try {
        return std::unique_lock<std::mutex>{mutex_};
    } catch (...) {
        fatal_watchdog_reboot(reason);
    }
}
#define RECOVERY_LOCK(stage, reason) lock_or_watchdog(reason)
#endif

void RecoveryCoordinator::fail_closed_locked(const char* reason) noexcept {
    slot_.fail_reason = reason;
    slot_.state = RecoverySlotState::FAIL_CLOSED;
}

bool RecoveryCoordinator::report_fault(const FaultReport& report) noexcept {
    const auto guard = RECOVERY_LOCK(REPORT_LOCK, "report_fault lock failed");
    {
        try {
        const auto make_fact = [this, &report](std::uint64_t sequence) {
            return FaultFact{FaultIdentity{slot_.boot_id, sequence}, report.kind,
                             report.subject, report.observed_epochs, report.evidence};
        };
        if (slot_.state == RecoverySlotState::IDLE) {
#ifdef RECOVERY_TESTING
            checkpoint(RuntimeFailureStage::REPORT_PREPARE);
#endif
            const auto next_sequence = slot_.recovery_seq + 1;
            const auto next_fault_sequence = slot_.fault_seq_counter + 1;
            RecoveryInstanceRuntime prepared{
                {slot_.boot_id, next_sequence, next_sequence},
                make_fact(next_fault_sequence)};
            slot_.active.emplace(std::move(prepared));
            slot_.fault_seq_counter = next_fault_sequence;
            slot_.recovery_seq = next_sequence;
            slot_.state = RecoverySlotState::PREPARING;
            return true;
        }
        if (slot_.state == RecoverySlotState::PREPARING
            || slot_.state == RecoverySlotState::FREEZING) {
            if (!slot_.active) {
                fail_closed_locked("active runtime missing"); return false;
            }
#ifdef RECOVERY_TESTING
            checkpoint(RuntimeFailureStage::REPORT_APPEND);
#endif
            const auto next_fault_sequence = slot_.fault_seq_counter + 1;
            auto prepared = make_fact(next_fault_sequence);
            slot_.active->fault_batch.push_back(std::move(prepared));
            slot_.fault_seq_counter = next_fault_sequence;
            return true;
        }
        if (slot_.state == RecoverySlotState::FAIL_CLOSED) return false;
        fail_closed_locked("fault after batch close");
        } catch (...) { fail_closed_locked("fault allocation failed"); }
    }
    return false;
}

std::optional<RecoveryCoordinator::PreparedFaultBatchClose>
RecoveryCoordinator::prepare_fault_batch_close() const {
    if (!slot_.active || slot_.active->fault_batch.empty()) return std::nullopt;
    PreparedFaultBatchClose prepared;
    prepared.fault_batch = slot_.active->fault_batch;
    prepared.fault_batch_cutoff = slot_.fault_seq_counter;
    std::sort(prepared.fault_batch.begin(), prepared.fault_batch.end(),
              [](const auto& left, const auto& right) { return left.id < right.id; });
    for (std::size_t index = 0; index < prepared.fault_batch.size(); ++index) {
        const auto& fact = prepared.fault_batch[index];
        if (fact.id.boot_id != slot_.boot_id
            || fact.id.fault_seq > prepared.fault_batch_cutoff
            || (index != 0 && !(prepared.fault_batch[index - 1].id < fact.id)))
            return std::nullopt;
    }
    if (prepared.fault_batch.back().id.fault_seq != prepared.fault_batch_cutoff)
        return std::nullopt;
    prepared.fault_batch_digest = canonical_digest(prepared.fault_batch);
    return prepared;
}

BeginFreezeResult RecoveryCoordinator::begin_freeze(
    const std::vector<FrozenGrantRecord>& ledger,
    const std::vector<OwnerExecutorBinding>& owner_bindings) noexcept {
    const auto guard = RECOVERY_LOCK(BEGIN_FREEZE_LOCK, "begin_freeze lock failed");
    if (slot_.state != RecoverySlotState::PREPARING)
        return BeginFreezeError{BeginFreezeErrorCode::WRONG_PHASE};
    if (!slot_.active) {
        fail_closed_locked("begin_freeze active missing");
        return BeginFreezeError{BeginFreezeErrorCode::INTERNAL_FAILURE};
    }
    if (slot_.freeze_seq == std::numeric_limits<FreezeSeq>::max()) {
        fail_closed_locked("freeze seq exhausted");
        return BeginFreezeError{BeginFreezeErrorCode::SEQUENCE_EXHAUSTED};
    }

    // Phase A: every fallible step builds locals only.
    struct PreparedBeginFreeze {
        FreezeSeq next_freeze_seq;
        GrantSeq grant_cutoff;
        std::vector<FrozenGrantRecord> ledger_prefix;
        Digest ledger_prefix_digest;
        std::vector<OwnerExecutorBinding> owner_bindings;
        FreezeRequest request;
    };
    std::optional<PreparedBeginFreeze> prepared;
    try {
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::BEGIN_FREEZE_PREPARE);
#endif
        auto prefix = ledger;
        auto bindings = owner_bindings;
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::BEGIN_FREEZE_AFTER_COPY);
#endif
        GrantSeq cutoff{};
        for (const auto& grant : prefix) cutoff = std::max(cutoff, grant.identity.seq);
        const Digest prefix_digest = canonical_digest(prefix);
        const FreezeSeq next = slot_.freeze_seq + 1;   // exhaustion excluded above
        const FreezeRequest request{slot_.active->recovery_id, next, slot_.boot_id,
                                    slot_.closing_generation, cutoff, prefix_digest};
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::BEGIN_FREEZE_AFTER_DIGEST);
#endif
        prepared.emplace(PreparedBeginFreeze{next, cutoff, std::move(prefix), prefix_digest,
                                             std::move(bindings), request});
    } catch (const std::bad_alloc&) {
        return BeginFreezeError{BeginFreezeErrorCode::ALLOCATION_FAILED};
    } catch (...) {
        fail_closed_locked("begin_freeze build failed");
        return BeginFreezeError{BeginFreezeErrorCode::INTERNAL_FAILURE};
    }

    // Phase B: nothrow moves and scalar writes; state is published last. The
    // request is returned, not stored: callers re-send this exact value.
    static_assert(std::is_nothrow_move_assignable_v<std::vector<FrozenGrantRecord>>);
    static_assert(std::is_nothrow_move_assignable_v<std::vector<OwnerExecutorBinding>>);
    static_assert(std::is_nothrow_constructible_v<BeginFreezeResult, FreezeRequest>);
    slot_.freeze_seq = prepared->next_freeze_seq;
    slot_.active->grant_cutoff = prepared->grant_cutoff;
    slot_.active->ledger_prefix = std::move(prepared->ledger_prefix);
    slot_.active->ledger_prefix_digest = prepared->ledger_prefix_digest;
    slot_.active->owner_executor_bindings = std::move(prepared->owner_bindings);
    slot_.state = RecoverySlotState::FREEZING;
    return prepared->request;
}

bool RecoveryCoordinator::commit_freeze() noexcept {
    const auto guard = RECOVERY_LOCK(FREEZE_LOCK, "commit_freeze lock failed");
    if (slot_.state == RecoverySlotState::FROZEN) {
        fail_closed_locked("freeze already published"); return false;
    }
    if (slot_.state != RecoverySlotState::FREEZING || !slot_.active) return false;
    if (slot_.active->current_snapshot || !slot_.active->process_exit_binding_registry.empty()) {
        fail_closed_locked("freeze already published"); return false;
    }
    try {
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::FREEZE_TARGETS);
        checkpoint(RuntimeFailureStage::FREEZE_OWNER_ACK);
#endif
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::FREEZE_BATCH_PREPARE);
#endif
        const auto prepared_fault_batch = prepare_fault_batch_close();
        if (!prepared_fault_batch) {
            fail_closed_locked("fault batch not freezeable"); return false;
        }
        std::set<OwnerId> all_owners;
        for (const auto& grant : slot_.active->ledger_prefix) all_owners.insert(grant.owner);
        if (all_owners.size() != slot_.active->owner_acks.size()) {
            fail_closed_locked("owner ack missing"); return false;
        }
        for (const auto owner : all_owners) {
            if (!slot_.active->owner_acks.contains(owner)) {
                fail_closed_locked("owner ack missing"); return false;
            }
        }
        if (canonical_digest(slot_.active->ledger_prefix)
            != slot_.active->ledger_prefix_digest) {
            fail_closed_locked("prefix digest"); return false;
        }
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::FREEZE_PREFIX_DIGEST);
        checkpoint(RuntimeFailureStage::FREEZE_BATCH_DIGEST);
#endif
        std::set<ProcessIdentity> targets;
        for (const auto& grant : slot_.active->ledger_prefix)
            for (const auto& target : grant.execution_targets)
                if (const auto* process = std::get_if<ProcessIdentity>(&target)) targets.insert(*process);
        for (const auto& fault : prepared_fault_batch->fault_batch)
            if (fault.kind == FaultKind::WORKER_PROCESS_EXIT && fault.subject.process)
                targets.insert(*fault.subject.process);

        std::vector<ProcessExitBindingDescriptor> descriptors;
        ProcessExitBindingRegistry registry;
        descriptors.reserve(targets.size());
        ProcessExitBindingSeq sequence{};
        for (const auto& process : targets) {
            const auto before = kernel_->read_start_generation(process.pid);
            if (!before) { fail_closed_locked("READ_BEFORE_FAILED"); return false; }
            if (*before != process.start_generation) { fail_closed_locked("GENERATION_MISMATCH_BEFORE"); return false; }
            OwnedFd fd = kernel_->pidfd_open(process.pid);
            if (!fd.valid()) { fail_closed_locked("PIDFD_OPEN_FAILED"); return false; }
            const auto after = kernel_->read_start_generation(process.pid);
            if (!after) { fail_closed_locked("READ_AFTER_FAILED"); return false; }
            if (*after != process.start_generation) { fail_closed_locked("GENERATION_MISMATCH_AFTER"); return false; }
            ProcessExitBindingDescriptor descriptor{{slot_.active->recovery_id, slot_.freeze_seq, sequence++}, process};
            descriptors.push_back(descriptor);
            registry.emplace(descriptor.id, OwnedProcessExitBinding{descriptor, std::move(fd)});
        }
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::FREEZE_AFTER_BINDINGS);
        checkpoint(RuntimeFailureStage::FREEZE_SNAPSHOT);
#endif
        FrozenRuntimeSnapshot snapshot{slot_.active->recovery_id, slot_.freeze_seq,
            slot_.boot_id, slot_.closing_generation, slot_.active->grant_cutoff,
            slot_.active->ledger_prefix, slot_.active->ledger_prefix_digest,
            slot_.active->owner_ack_canonical_digest, prepared_fault_batch->fault_batch,
            prepared_fault_batch->fault_batch_cutoff, prepared_fault_batch->fault_batch_digest,
            slot_.active->owner_executor_bindings, std::move(descriptors),
            slot_.epochs, {}};
        if (!process_exit_bindings_are_legal(snapshot)
            || !descriptors_registry_agree(snapshot.process_exit_bindings, registry)) {
            fail_closed_locked("process exit bindings illegal"); return false;
        }
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::FREEZE_BINDING_LEGALITY);
#endif
        snapshot.snapshot_digest = snapshot_digest(snapshot);
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::FREEZE_SNAPSHOT_DIGEST);
#endif
        static_assert(std::is_nothrow_move_assignable_v<ProcessExitBindingRegistry>);
        static_assert(std::is_nothrow_move_assignable_v<FrozenRuntimeSnapshot>);
        static_assert(std::is_nothrow_move_assignable_v<std::optional<FrozenRuntimeSnapshot>>);
        slot_.active->process_exit_binding_registry = std::move(registry);
        slot_.active->current_snapshot = std::move(snapshot);
        slot_.state = RecoverySlotState::FROZEN;
        return true;
    } catch (...) { fail_closed_locked("freeze commit build failed"); return false; }
}

bool RecoveryCoordinator::submit_owner_snapshot_ack(const OwnerSnapshotAck& ack) noexcept {
    const auto guard = RECOVERY_LOCK(OWNER_ACK_LOCK, "submit_owner_snapshot_ack lock failed");
    if (slot_.state != RecoverySlotState::FREEZING || !slot_.active) return false;
    try {
        if (ack.recovery_id != slot_.active->recovery_id
            || ack.freeze_seq != slot_.freeze_seq || ack.boot_id != slot_.boot_id
            || ack.closing_generation != slot_.closing_generation
            || owner_ack_view_digest(ack) != ack.local_view_digest
            || slot_.active->owner_acks.contains(ack.owner)) return false;
        std::set<GrantIdentity> expected_grants;
        std::set<LeaseIdentity> expected_leases;
        for (const auto& grant : slot_.active->ledger_prefix) {
            if (grant.owner != ack.owner) continue;
            expected_grants.insert(grant.identity);
            if (grant.lease) expected_leases.insert(*grant.lease);
        }
        if (expected_grants.empty() || ack.locally_observed_grants != expected_grants
            || ack.outstanding_leases != expected_leases) return false;
        auto candidate_acks = slot_.active->owner_acks;
        candidate_acks.emplace(ack.owner, ack);
        std::vector<OwnerSnapshotAck> canonical;
        canonical.reserve(candidate_acks.size());
        for (const auto& entry : candidate_acks) canonical.push_back(entry.second);
        const auto candidate_digest = canonical_digest(canonical);
        static_assert(std::is_nothrow_move_assignable_v<decltype(candidate_acks)>);
        slot_.active->owner_acks = std::move(candidate_acks);
        slot_.active->owner_ack_canonical_digest = candidate_digest;
        return true;
    } catch (...) { fail_closed_locked("owner ack allocation failed"); return false; }
}

bool RecoveryCoordinator::begin_compile(PolicySnapshot policy,
                                         CapabilityRegistrySnapshot registry) noexcept {
    const auto guard = RECOVERY_LOCK(BEGIN_COMPILE_LOCK, "begin_compile lock failed");
    if (slot_.state != RecoverySlotState::FROZEN || !slot_.active) {
        fail_closed_locked("state"); return false;
    }
    try {
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::BEGIN_COMPILE_PUBLISH);
#endif
        PolicySnapshot prepared_policy = std::move(policy);
        CapabilityRegistrySnapshot prepared_registry = std::move(registry);
        static_assert(std::is_nothrow_move_assignable_v<PolicySnapshot>);
        static_assert(std::is_nothrow_move_assignable_v<CapabilityRegistrySnapshot>);
        slot_.active->current_policy = std::move(prepared_policy);
        slot_.active->current_registry = std::move(prepared_registry);
        slot_.state = RecoverySlotState::COMPILING;
        return true;
    } catch (...) { fail_closed_locked("begin_compile build failed"); return false; }
}

void RecoveryCoordinator::finish_compile(CompileResult result) noexcept {
    const auto guard = RECOVERY_LOCK(FINISH_COMPILE_LOCK, "finish_compile lock failed");
    if (slot_.state != RecoverySlotState::COMPILING || !slot_.active) {
        fail_closed_locked("state"); return;
    }
    try {
        if (auto* failure = std::get_if<CompileFailure>(&result)) {
            if (failure->kind != CompileFailure::Kind::FALLBACK_REQUIRED) {
                fail_closed_locked("compile rejected"); return;
            }
            if (!failure->fallback) { fail_closed_locked("no fallback action"); return; }
            auto prepared_fallback = failure->fallback;
            static_assert(std::is_nothrow_move_assignable_v<std::optional<FallbackAction>>);
            slot_.active->pending_fallback = std::move(prepared_fallback);
            slot_.active->fallback_pending = true;
            return;
        }
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::FINISH_COMPILE_INSTALL);
#endif
        auto program = std::make_shared<const ValidatedRecoveryProgram>(
            std::get<ValidatedRecoveryProgram>(std::move(result)));
        if (!slot_.active->current_snapshot
            || !snapshot_contents_match(*slot_.active)
            || program->snapshot_digest() != slot_.active->current_snapshot->snapshot_digest
            || program->policy_digest() != slot_.active->current_policy.digest
            || program->registry_digest() != slot_.active->current_registry.digest) {
            fail_closed_locked("compiled input mismatch"); return;
        }
        slot_.active->compiled_program = std::move(program);
    } catch (...) { fail_closed_locked("finish_compile install failed"); }
}

void RecoveryCoordinator::execute_fallback(std::optional<FallbackAction> fallback) noexcept {
    const auto guard = RECOVERY_LOCK(FALLBACK_LOCK, "execute_fallback lock failed");
    if (slot_.state != RecoverySlotState::COMPILING || !slot_.active
        || !slot_.active->fallback_pending
        || fallback != slot_.active->pending_fallback) {
        fail_closed_locked("state"); return;
    }
    fail_closed_locked("fallback");
}

bool RecoveryCoordinator::seal_validated_program(
    const std::shared_ptr<const ValidatedRecoveryProgram>& program) noexcept {
    const auto guard = RECOVERY_LOCK(SEAL_LOCK, "seal lock failed");
    if (slot_.state != RecoverySlotState::COMPILING || !slot_.active
        || !program || program.get() != slot_.active->compiled_program.get()
        || !slot_.active->current_snapshot) {
        fail_closed_locked("seal validation"); return false;
    }
    try {
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::SEAL_VERIFY);
#endif
        if (compute_transaction_hash(*program) != program->transaction_hash()
            || snapshot_digest(*slot_.active->current_snapshot)
                 != slot_.active->current_snapshot->snapshot_digest
            || program->snapshot_digest() != slot_.active->current_snapshot->snapshot_digest
            || program->policy_digest() != slot_.active->current_policy.digest
            || program->registry_digest() != slot_.active->current_registry.digest
            || program->faults() != slot_.active->current_snapshot->fault_batch) {
            fail_closed_locked("seal validation"); return false;
        }
        if (!snapshot_contents_match(*slot_.active)
            || !expected_executors_match(*program, *slot_.active->current_snapshot,
                                         slot_.active->current_policy)) {
            fail_closed_locked("seal contents validation"); return false;
        }
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::SEAL_PUBLISH);
#endif
        const TransactionHash hash = program->transaction_hash();
        slot_.active->current_program = slot_.active->compiled_program;
        slot_.active->transaction_hash = hash;
        slot_.state = RecoverySlotState::SEALED;
        return true;
    } catch (...) { fail_closed_locked("seal verification failed"); return false; }
}
}  // namespace recovery
