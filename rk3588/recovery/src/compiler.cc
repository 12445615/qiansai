#include "recovery/compiler.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>

namespace recovery {
namespace {

std::uint64_t epoch_component(const EpochSnapshot& epochs, EpochDomain domain) {
    switch (domain) {
    case EpochDomain::STREAM: return epochs.stream_epoch;
    case EpochDomain::AI: return epochs.ai_epoch;
    case EpochDomain::WORKER: return epochs.worker_epoch;
    case EpochDomain::MEDIA: return epochs.media_epoch;
    }
    return 0;
}

std::optional<EpochSnapshot> advance_epochs(
    const EpochSnapshot& epochs, const std::set<EpochDomain>& domains) {
    EpochSnapshot result = epochs;
    for (const auto domain : domains) {
        const auto value = epoch_component(epochs, domain);
        if (value == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
        switch (domain) {
        case EpochDomain::STREAM: result.stream_epoch = value + 1; break;
        case EpochDomain::AI: result.ai_epoch = value + 1; break;
        case EpochDomain::WORKER: result.worker_epoch = value + 1; break;
        case EpochDomain::MEDIA: result.media_epoch = value + 1; break;
        }
    }
    return result;
}

std::optional<EpochDomain> first_overflowing_domain(
    const EpochSnapshot& epochs, const std::set<EpochDomain>& domains) {
    for (const auto domain : domains) {
        if (epoch_component(epochs, domain) == std::numeric_limits<std::uint64_t>::max()) {
            return domain;
        }
    }
    return std::nullopt;
}

const OwnerExecutorBinding* binding_for(
    const FrozenRuntimeSnapshot& snapshot, ResourceInstance resource) {
    const auto found = std::find_if(snapshot.owner_executor_bindings.begin(),
                                    snapshot.owner_executor_bindings.end(),
                                    [resource](const auto& binding) {
                                        return binding.resource == resource;
                                    });
    return found == snapshot.owner_executor_bindings.end() ? nullptr : &*found;
}

std::size_t binding_count(const FrozenRuntimeSnapshot& snapshot, ResourceInstance resource) {
    return static_cast<std::size_t>(std::count_if(
        snapshot.owner_executor_bindings.begin(), snapshot.owner_executor_bindings.end(),
        [resource](const auto& binding) { return binding.resource == resource; }));
}

std::variant<ExecutorPrincipal, ActionClosureFailure> expected_executor(
    const RecoveryResourcePlan& plan, const FrozenRuntimeSnapshot& snapshot) {
    const auto* binding = binding_for(snapshot, plan.resource);
    if (binding == nullptr) {
        return ActionClosureFailure{ActionClosureFailureKind::EXECUTOR_BINDING_MISSING,
                                    plan.resource, std::nullopt};
    }
    if (binding->status == ExecutorBindingStatus::AMBIGUOUS) {
        return ActionClosureFailure{ActionClosureFailureKind::EXECUTOR_BINDING_AMBIGUOUS,
                                    plan.resource, std::nullopt};
    }
    if (binding_count(snapshot, plan.resource) != 1) {
        return ActionClosureFailure{ActionClosureFailureKind::EXECUTOR_BINDING_AMBIGUOUS,
                                    plan.resource, std::nullopt};
    }
    if (binding->status == ExecutorBindingStatus::UNAUTHENTICATABLE
        || !binding->principal
        || !std::holds_alternative<ProcessPrincipal>(*binding->principal)) {
        return ActionClosureFailure{ActionClosureFailureKind::EXECUTOR_BINDING_UNAUTHENTICATABLE,
                                    plan.resource, std::nullopt};
    }
    return *binding->principal;
}

std::vector<ActionNode> canonicalize(std::vector<ActionNode> nodes) {
    std::vector<std::size_t> order(nodes.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&nodes](std::size_t left, std::size_t right) {
        const auto& a = nodes[left];
        const auto& b = nodes[right];
        return std::tie(a.phase, a.scope, a.operation, a.old_epochs, a.new_epochs,
                        a.expected_executor)
             < std::tie(b.phase, b.scope, b.operation, b.old_epochs, b.new_epochs,
                        b.expected_executor);
    });
    std::vector<ActionId> ids(nodes.size());
    for (std::size_t index = 0; index < order.size(); ++index) ids[order[index]] = index;
    std::vector<ActionNode> result;
    result.reserve(nodes.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
        ActionNode node = std::move(nodes[order[index]]);
        std::set<ActionId> prerequisites;
        for (const auto prior : node.prerequisites) prerequisites.insert(ids.at(prior));
        node.id = index;
        node.prerequisites = std::move(prerequisites);
        result.push_back(std::move(node));
    }
    return result;
}

std::optional<std::pair<ResourceKind, RecoveryMode>> resource_for(FaultKind kind) {
    switch (kind) {
    case FaultKind::V4L2_DEVICE_FAULT: return {{ResourceKind::V4L2_CAPTURE, RecoveryMode::CAPTURE_RESET}};
    case FaultKind::RGA_DEVICE_FAULT: return {{ResourceKind::RGA, RecoveryMode::ENCODING_RESET}};
    case FaultKind::RKNN_DEVICE_FAULT: return {{ResourceKind::RKNN_AI, RecoveryMode::AI_RESET}};
    case FaultKind::MPP_DEVICE_FAULT: return {{ResourceKind::MPP_ENCODER, RecoveryMode::ENCODING_RESET}};
    case FaultKind::PACKET_DOMAIN_FAULT: return {{ResourceKind::PACKET_DOMAIN, RecoveryMode::PACKET_RESET}};
    case FaultKind::RTMP_SINK_FAULT: return {{ResourceKind::RTMP_SINK, RecoveryMode::RTMP_SINK_RESET}};
    case FaultKind::RECORD_SINK_FAULT: return {{ResourceKind::RECORD_SINK, RecoveryMode::RECORD_SINK_RESET}};
    case FaultKind::WORKER_PROCESS_EXIT: return {{ResourceKind::WORKER_PROCESS, RecoveryMode::WORKER_RESTART}};
    case FaultKind::CAPABILITY_FAULT:
    case FaultKind::UNKNOWN: return std::nullopt;
    }
    return std::nullopt;
}

bool dag_valid(const std::vector<ActionNode>& dag) {
    if (dag.empty()) return false;
    std::set<ActionId> ids;
    std::size_t commits = 0;
    std::size_t reopens = 0;
    std::set<ActionId> local_complete;
    const ActionNode* commit = nullptr;
    const ActionNode* reopen = nullptr;
    for (const auto& node : dag) {
        if (!ids.insert(node.id).second || node.prerequisites.contains(node.id)) return false;
        if (node.phase == ActionPhase::LOCAL_COMPLETE) local_complete.insert(node.id);
        if (node.phase == ActionPhase::COMMIT_EPOCH) { ++commits; commit = &node; }
        if (node.phase == ActionPhase::REOPEN_ADMISSION) { ++reopens; reopen = &node; }
    }
    for (const auto& node : dag) {
        for (const auto prior : node.prerequisites) if (!ids.contains(prior)) return false;
    }
    if (commits != 1 || reopens != 1 || commit == nullptr || reopen == nullptr
        || commit->scope != ActionScope{GlobalScope{}}
        || reopen->scope != ActionScope{GlobalScope{}}
        || commit->expected_executor != kCoordinatorPrincipal
        || reopen->expected_executor != kCoordinatorPrincipal
        || commit->prerequisites != local_complete
        || reopen->prerequisites != std::set<ActionId>{commit->id}) {
        return false;
    }
    std::map<ActionId, const ActionNode*> by_id;
    for (const auto& node : dag) by_id.emplace(node.id, &node);
    std::map<ActionId, unsigned char> color;
    const auto visit = [&](const auto& self, ActionId id) -> bool {
        auto& state = color[id];
        if (state == 1) return false;
        if (state == 2) return true;
        state = 1;
        for (const auto prerequisite : by_id.at(id)->prerequisites) {
            if (!self(self, prerequisite)) return false;
        }
        state = 2;
        return true;
    };
    for (const auto& node : dag) if (!visit(visit, node.id)) return false;
    std::set<ActionId> commit_ancestors;
    const auto collect = [&](const auto& self, ActionId id) -> void {
        if (!commit_ancestors.insert(id).second) return;
        for (const auto prerequisite : by_id.at(id)->prerequisites) self(self, prerequisite);
    };
    collect(collect, commit->id);
    std::map<ActionId, std::size_t> dependent_count;
    for (const auto& node : dag) {
        dependent_count.try_emplace(node.id, 0);
        for (const auto prerequisite : node.prerequisites) ++dependent_count[prerequisite];
    }
    for (const auto& node : dag) {
        if (node.id == commit->id || node.id == reopen->id) continue;
        if (std::holds_alternative<GlobalScope>(node.scope)
            || std::holds_alternative<CoordinatorPrincipal>(node.expected_executor)
            || !commit_ancestors.contains(node.id)) {
            return false;
        }
    }
    return dependent_count[reopen->id] == 0
        && std::count_if(dependent_count.begin(), dependent_count.end(),
                         [](const auto& entry) { return entry.second == 0; }) == 1;
}

bool epoch_domains_consistent(const std::vector<RecoveryResourcePlan>& plans) {
    std::map<EpochDomain, std::uint64_t> observed;
    for (const auto& plan : plans) {
        for (const auto domain : plan.invalidated_domains) {
            const auto value = epoch_component(plan.old_epochs, domain);
            const auto [found, inserted] = observed.emplace(domain, value);
            if (!inserted && found->second != value) return false;
        }
    }
    return true;
}

std::set<DeviceTarget> resource_device_closure(
    const FrozenRuntimeSnapshot& snapshot, ResourceInstance resource) {
    std::set<DeviceTarget> result;
    for (const auto& grant : snapshot.ledger_prefix) {
        if (grant.source == resource || grant.destination == resource) {
            result.insert(grant.device_targets.begin(), grant.device_targets.end());
        }
    }
    return result;
}

std::set<SubjectIdentity> resource_proof_subject_closure(
    const FrozenRuntimeSnapshot& snapshot, ResourceInstance resource) {
    std::set<SubjectIdentity> result;
    for (const auto& grant : snapshot.ledger_prefix) {
        if (grant.source == resource || grant.destination == resource) {
            result.insert(grant.execution_targets.begin(), grant.execution_targets.end());
        }
    }
    return result;
}

CapabilityExpectation expectation_from(const CapabilityEvidence& evidence) {
    return {evidence.capability_id, evidence.device_context, evidence.environment,
            evidence.evidence_id, evidence.evidence_digest,
            evidence.valid_from, evidence.valid_until};
}

}  // namespace

bool process_exit_bindings_are_legal(const FrozenRuntimeSnapshot& snapshot) noexcept {
    const auto& bindings = snapshot.process_exit_bindings;
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        const auto& binding = bindings[i];
        if (binding.id.recovery_id != snapshot.recovery_id
            || binding.id.freeze_seq != snapshot.freeze_seq
            || binding.id.binding_seq != i) {
            return false;
        }
        if (i > 0 && !(bindings[i - 1].process < binding.process)) return false;
        for (std::size_t j = 0; j < i; ++j) {
            if (bindings[j].process == binding.process || bindings[j].id == binding.id) {
                return false;
            }
        }
    }
    return true;
}

const ProcessExitBindingDescriptor* find_process_exit_binding(
    const FrozenRuntimeSnapshot& snapshot, const ProcessIdentity& process) noexcept {
    const ProcessExitBindingDescriptor* result = nullptr;
    for (const auto& binding : snapshot.process_exit_bindings) {
        if (binding.process != process) continue;
        if (result != nullptr) return nullptr;
        result = &binding;
    }
    return result;
}

PredicateDerivation derive_predicates(
    const std::vector<RecoveryResourcePlan>& plans,
    const FrozenRuntimeSnapshot& snapshot,
    const PolicySnapshot& policy,
    const CapabilityRegistrySnapshot& registry,
    TrustedMonotonicTime now) {
    if (!process_exit_bindings_are_legal(snapshot)) {
        return PredicateDerivationFailure{
            PredicateDerivationFailureKind::EXECUTION_TARGET_UNRESOLVED,
            std::nullopt, std::nullopt};
    }
    if (!policy.contents || canonical_digest(*policy.contents) != policy.digest
        || !registry.contents || canonical_digest(*registry.contents) != registry.digest) {
        return PredicateDerivationFailure{
            PredicateDerivationFailureKind::REGISTRY_DIGEST_MISMATCH,
            std::nullopt, std::nullopt};
    }

    std::set<DeviceContextIdentity> device_contexts;
    std::set<SubjectIdentity> execution_subjects;
    std::map<DeviceContextIdentity, ResourceKind> device_resources;
    std::map<SubjectIdentity, EpochMask> execution_masks;
    for (const auto& plan : plans) {
        const auto mask = policy.contents->resource_epoch_mask.contains(plan.resource_kind)
            ? policy.contents->resource_epoch_mask.at(plan.resource_kind) : EpochMask{0};
        for (const auto& target : resource_device_closure(snapshot, plan.resource)) {
            device_contexts.insert(target.device_context);
            device_resources.emplace(target.device_context, plan.resource_kind);
        }
        for (const auto& subject : resource_proof_subject_closure(snapshot, plan.resource)) {
            execution_subjects.insert(subject);
            execution_masks[subject] |= mask;
        }
    }
    for (const auto& fault : snapshot.fault_batch) {
        if (fault.subject.device_context
            && !device_contexts.contains(*fault.subject.device_context)) {
            return PredicateDerivationFailure{
                PredicateDerivationFailureKind::TARGET_NOT_IN_CLOSURE,
                fault.subject.device_context, std::nullopt};
        }
        if (fault.kind == FaultKind::WORKER_PROCESS_EXIT && fault.subject.process) {
            execution_subjects.insert(SubjectIdentity{*fault.subject.process});
        }
    }

    DerivedPredicates result;
    for (const auto& subject : execution_subjects) {
        const auto kind = std::holds_alternative<ThreadIdentity>(subject)
            ? ExecutionProofKind::THREADS_JOINED
            : ExecutionProofKind::PROCESS_EXIT_CONFIRMED;
        ExecutionPredicate predicate{PredicateKey{ExecutionPredicateKey{kind, subject}},
                                     kind, subject, execution_masks[subject],
                                     std::nullopt, std::nullopt};
        if (const auto* process = std::get_if<ProcessIdentity>(&subject)) {
            const auto* binding = find_process_exit_binding(snapshot, *process);
            if (binding == nullptr) {
                return PredicateDerivationFailure{
                    PredicateDerivationFailureKind::EXECUTION_TARGET_UNRESOLVED,
                    std::nullopt, subject};
            }
            predicate.expected_process_binding = binding->id;
            predicate.expected_spawn_generation = process->start_generation;
        }
        result.execution.push_back(std::move(predicate));
    }

    for (const auto device : device_contexts) {
        const auto capability = policy.contents->device_capability_map.find(device);
        const auto resource = device_resources.find(device);
        if (capability == policy.contents->device_capability_map.end()
            || resource == device_resources.end()) {
            return PredicateDerivationFailure{
                PredicateDerivationFailureKind::RULE_MISSING, device, std::nullopt};
        }
        const auto proof_kind =
            policy.contents->resource_device_proof_kind.find(resource->second);
        if (proof_kind == policy.contents->resource_device_proof_kind.end()) {
            return PredicateDerivationFailure{
                PredicateDerivationFailureKind::RULE_MISSING, device, std::nullopt};
        }

        std::vector<const CapabilityEvidence*> usable;
        bool revoked = false;
        bool expired = false;
        for (const auto& evidence : registry.contents->evidences) {
            if (evidence.device_context != device
                || evidence.capability_id != capability->second) continue;
            if (registry.contents->revoked_evidence.contains(evidence.evidence_id)) {
                revoked = true;
            } else if (now < evidence.valid_from || now > evidence.valid_until) {
                expired = true;
            } else {
                usable.push_back(&evidence);
            }
        }
        if (usable.size() > 1) {
            return PredicateDerivationFailure{
                PredicateDerivationFailureKind::EVIDENCE_AMBIGUOUS, device, std::nullopt};
        }
        if (usable.empty()) {
            const auto kind = revoked ? PredicateDerivationFailureKind::EVIDENCE_REVOKED
                : expired ? PredicateDerivationFailureKind::EVIDENCE_EXPIRED
                : PredicateDerivationFailureKind::EVIDENCE_MISSING;
            return PredicateDerivationFailure{kind, device, std::nullopt};
        }
        const auto expectation = expectation_from(*usable.front());
        const auto mask = policy.contents->resource_epoch_mask.contains(resource->second)
            ? policy.contents->resource_epoch_mask.at(resource->second) : EpochMask{0};
        result.device.push_back(DevicePredicate{
            PredicateKey{DevicePredicateKey{proof_kind->second, device}}, proof_kind->second,
            device, DeviceTarget{device}, mask, expectation});
    }
    return result;
}

ValidatedRecoveryProgram::ValidatedRecoveryProgram(
    RecoveryInstanceId recovery_id, FreezeSeq freeze_seq, BootId boot_id,
    Digest snapshot_digest, Digest policy_digest,
    CapabilityRegistryVersion registry_version, Digest registry_digest,
    std::vector<FaultFact> faults, std::vector<RecoveryResourcePlan> resource_plans,
    FaultCoverage fault_coverage, std::vector<ExecutionPredicate> execution_predicates,
    std::vector<DevicePredicate> device_predicates,
    std::vector<CapabilityExpectation> capability_expectations,
    std::vector<ActionNode> action_dag)
    : recovery_id_(recovery_id), freeze_seq_(freeze_seq), boot_id_(boot_id),
      snapshot_digest_(snapshot_digest), policy_digest_(policy_digest),
      registry_version_(registry_version), registry_digest_(registry_digest),
      faults_(std::move(faults)), resource_plans_(std::move(resource_plans)),
      fault_coverage_(std::move(fault_coverage)),
      execution_predicates_(std::move(execution_predicates)),
      device_predicates_(std::move(device_predicates)),
      capability_expectations_(std::move(capability_expectations)),
      action_dag_(std::move(action_dag)) {
    transaction_hash_ = compute_transaction_hash(*this);
}

Digest compute_transaction_hash(const ValidatedRecoveryProgram& program) {
    CanonicalWriter writer;
    canonical_write(writer, program.recovery_id());
    canonical_write(writer, program.freeze_seq());
    canonical_write(writer, program.boot_id());
    canonical_write(writer, program.snapshot_digest());
    canonical_write(writer, program.policy_digest());
    canonical_write(writer, program.registry_version());
    canonical_write(writer, program.registry_digest());
    canonical_write(writer, program.faults());
    canonical_write(writer, program.resource_plans());
    canonical_write(writer, program.fault_coverage());
    canonical_write(writer, program.execution_predicates());
    canonical_write(writer, program.device_predicates());
    canonical_write(writer, program.capability_expectations());
    canonical_write(writer, program.action_dag());
    return digest_bytes(std::move(writer).finish());
}

ActionClosureResult canonical_action_closure(
    const std::vector<RecoveryResourcePlan>& plans,
    const FrozenRuntimeSnapshot& snapshot,
    const PolicySnapshot& policy) {
    std::vector<ActionNode> dag;
    std::vector<ActionId> terminals;
    std::set<EpochDomain> all_domains;
    for (const auto& plan : plans) {
        const auto recipe = policy.contents->recipes.find({plan.resource_kind, plan.mode});
        if (recipe == policy.contents->recipes.end() || recipe->second.actions.empty()) {
            return ActionClosureFailure{ActionClosureFailureKind::RECIPE_MISSING,
                                        plan.resource, std::nullopt};
        }
        if (recipe->second.resource_kind != plan.resource_kind
            || recipe->second.recovery_mode != plan.mode) {
            return ActionClosureFailure{ActionClosureFailureKind::RECIPE_MISSING,
                                        plan.resource, std::nullopt};
        }
        const auto executor = expected_executor(plan, snapshot);
        if (const auto* failure = std::get_if<ActionClosureFailure>(&executor)) return *failure;
        std::vector<ActionId> branch;
        for (const auto& spec : recipe->second.actions) {
            ActionNode node{static_cast<ActionId>(dag.size()), spec.phase, spec.operation,
                            ActionScope{plan.resource}, plan.old_epochs, std::nullopt, {},
                            std::get<ExecutorPrincipal>(executor)};
            if (spec.operation == OperationKind::INVALIDATE_EPOCH) {
                node.new_epochs = advance_epochs(plan.old_epochs, plan.invalidated_domains);
                if (!node.new_epochs) {
                    return ActionClosureFailure{ActionClosureFailureKind::EPOCH_OVERFLOW,
                                                plan.resource,
                                                first_overflowing_domain(plan.old_epochs,
                                                                         plan.invalidated_domains)};
                }
            }
            for (const auto index : spec.prerequisite_indices) {
                if (index >= branch.size()) {
                    return ActionClosureFailure{ActionClosureFailureKind::RECIPE_MISSING,
                                                plan.resource, std::nullopt};
                }
                node.prerequisites.insert(branch[index]);
            }
            branch.push_back(node.id);
            dag.push_back(std::move(node));
        }
        ActionNode complete{static_cast<ActionId>(dag.size()), ActionPhase::LOCAL_COMPLETE,
                            OperationKind::LOCAL_COMPLETE, ActionScope{plan.resource},
                            plan.old_epochs, std::nullopt, {branch.back()},
                            std::get<ExecutorPrincipal>(executor)};
        terminals.push_back(complete.id);
        dag.push_back(std::move(complete));
        all_domains.insert(plan.invalidated_domains.begin(), plan.invalidated_domains.end());
    }
    const auto new_epochs = advance_epochs(snapshot.epoch_snapshot, all_domains);
    if (!new_epochs) {
        return ActionClosureFailure{ActionClosureFailureKind::EPOCH_OVERFLOW, std::nullopt,
                                    first_overflowing_domain(snapshot.epoch_snapshot, all_domains)};
    }
    ActionNode commit{static_cast<ActionId>(dag.size()), ActionPhase::COMMIT_EPOCH,
                      OperationKind::COMMIT_EPOCH, ActionScope{GlobalScope{}},
                      snapshot.epoch_snapshot, new_epochs,
                      std::set<ActionId>{terminals.begin(), terminals.end()}, kCoordinatorPrincipal};
    dag.push_back(commit);
    dag.push_back(ActionNode{static_cast<ActionId>(dag.size()), ActionPhase::REOPEN_ADMISSION,
                             OperationKind::REOPEN_ADMISSION, ActionScope{GlobalScope{}},
                             snapshot.epoch_snapshot, std::nullopt, {commit.id},
                             kCoordinatorPrincipal});
    return canonicalize(std::move(dag));
}

CompileResult RecoveryCompiler::compile(
    const FrozenRuntimeSnapshot& snapshot, const PolicySnapshot& policy,
    const CapabilityRegistrySnapshot& registry, TrustedMonotonicTime now) noexcept {
    NoopCompilerAllocationBoundary boundary;
    return compile_impl(snapshot, policy, registry, now, boundary);
}

CompileResult RecoveryCompiler::compile_impl(
    const FrozenRuntimeSnapshot& snapshot, const PolicySnapshot& policy,
    const CapabilityRegistrySnapshot& registry, TrustedMonotonicTime now,
    CompilerAllocationBoundary& allocation_boundary) noexcept {
    try {
        if (snapshot.fault_batch.empty()) return CompileFailure{CompileFailure::Kind::EMPTY_FAULT_BATCH};
        if (!policy.contents || canonical_digest(*policy.contents) != policy.digest
            || !registry.contents || canonical_digest(*registry.contents) != registry.digest) {
            CompileFailure failure{CompileFailure::Kind::PREDICATE_DERIVATION_FAILED};
            failure.predicate_failure = PredicateDerivationFailure{
                PredicateDerivationFailureKind::REGISTRY_DIGEST_MISMATCH,
                std::nullopt, std::nullopt};
            return failure;
        }
        allocation_boundary.checkpoint(CompilerAllocationStage::DERIVE_PLANS);
        std::map<ResourceInstance, RecoveryResourcePlan> plans_by_resource;
        FaultCoverage coverage;
        std::optional<FallbackAction> fallback;
        bool plan_conflict = false;
        for (const auto& fault : snapshot.fault_batch) {
            auto disposition = policy.contents->dispositions.find(fault.kind);
            const auto resource = resource_for(fault.kind);
            if (disposition == policy.contents->dispositions.end() || !resource
                || disposition->second == FaultDisposition::UNRECOVERABLE) {
                const auto fallback_kind = policy.contents->fallbacks.find(fault.kind);
                if (fallback_kind != policy.contents->fallbacks.end()) {
                    if (!fallback
                        || fallback_kind->second == FallbackKind::BOARD_RESTART_REQUIRED) {
                        if (!fallback || fallback->kind != FallbackKind::BOARD_RESTART_REQUIRED) {
                            const auto prior = fallback
                                ? fallback->triggering_faults : std::set<FaultIdentity>{};
                            fallback = FallbackAction{fallback_kind->second, prior};
                        }
                    }
                    fallback->triggering_faults.insert(fault.id);
                }
                std::optional<FallbackAction> entry_fallback;
                if (fallback_kind != policy.contents->fallbacks.end()) {
                    entry_fallback = FallbackAction{fallback_kind->second, {fault.id}};
                }
                coverage.entries.push_back(
                    {fault.id, FaultDisposition::UNRECOVERABLE, {}, entry_fallback});
                continue;
            }
            const auto plan = RecoveryResourcePlan{fault.subject.resource, resource->first,
                                                   resource->second, fault.observed_epochs,
                                                   resource->first == ResourceKind::WORKER_PROCESS
                                                       ? std::set<EpochDomain>{EpochDomain::WORKER}
                                                       : resource->first == ResourceKind::RKNN_AI
                                                           ? std::set<EpochDomain>{EpochDomain::AI}
                                                           : resource->first == ResourceKind::RGA
                                                               || resource->first == ResourceKind::MPP_ENCODER
                                                               || resource->first == ResourceKind::RTMP_SINK
                                                               || resource->first == ResourceKind::RECORD_SINK
                                                               ? std::set<EpochDomain>{EpochDomain::MEDIA}
                                                               : std::set<EpochDomain>{EpochDomain::STREAM}};
            const auto [existing, inserted] = plans_by_resource.emplace(plan.resource, plan);
            if (!inserted && existing->second != plan) plan_conflict = true;
            coverage.entries.push_back({fault.id, disposition->second, {plan.resource}, std::nullopt});
        }
        if (std::any_of(coverage.entries.begin(), coverage.entries.end(), [](const auto& entry) {
                return entry.disposition == FaultDisposition::UNRECOVERABLE;
            })) {
            return CompileFailure{CompileFailure::Kind::FALLBACK_REQUIRED, fallback};
        }
        std::vector<RecoveryResourcePlan> plans;
        for (auto& [resource, plan] : plans_by_resource) {
            (void)resource;
            plans.push_back(std::move(plan));
        }
        if (plans.empty()) return CompileFailure{CompileFailure::Kind::EMPTY_PLAN};
        if (plan_conflict || !epoch_domains_consistent(plans)) {
            return CompileFailure{CompileFailure::Kind::INVALID_DAG};
        }
        allocation_boundary.checkpoint(CompilerAllocationStage::ACTION_CLOSURE);
        auto closure = canonical_action_closure(plans, snapshot, policy);
        if (const auto* failure = std::get_if<ActionClosureFailure>(&closure)) {
            CompileFailure result{CompileFailure::Kind::ACTION_CLOSURE_FAILED};
            result.closure_failure = *failure;
            return result;
        }
        auto dag = std::get<std::vector<ActionNode>>(std::move(closure));
        if (!dag_valid(dag)) return CompileFailure{CompileFailure::Kind::INVALID_DAG};
        allocation_boundary.checkpoint(CompilerAllocationStage::DERIVE_PREDICATES);
        auto predicates = derive_predicates(plans, snapshot, policy, registry, now);
        if (const auto* failure = std::get_if<PredicateDerivationFailure>(&predicates)) {
            CompileFailure result{CompileFailure::Kind::PREDICATE_DERIVATION_FAILED};
            result.predicate_failure = *failure;
            return result;
        }
        auto derived = std::get<DerivedPredicates>(std::move(predicates));
#ifdef RECOVERY_TESTING
        allocation_boundary.mutate_predicates_for_test(derived);
#endif
        std::set<SubjectIdentity> execution_targets;
        std::set<DeviceContextIdentity> device_targets;
        for (const auto& plan : plans) {
            const auto execution = resource_proof_subject_closure(snapshot, plan.resource);
            execution_targets.insert(execution.begin(), execution.end());
            for (const auto& target : resource_device_closure(snapshot, plan.resource)) {
                device_targets.insert(target.device_context);
            }
        }
        for (const auto& fault : snapshot.fault_batch) {
            if (fault.kind == FaultKind::WORKER_PROCESS_EXIT && fault.subject.process) {
                execution_targets.insert(SubjectIdentity{*fault.subject.process});
            }
        }
        std::set<SubjectIdentity> execution_covered;
        for (const auto& predicate : derived.execution) {
            if (!execution_covered.insert(predicate.expected_subject).second) {
                return CompileFailure{CompileFailure::Kind::TARGET_PREDICATE_MISMATCH};
            }
        }
        std::set<DeviceContextIdentity> device_covered;
        std::vector<CapabilityExpectation> expectations;
        for (const auto& predicate : derived.device) {
            if (!device_covered.insert(predicate.expected_device_context).second) {
                return CompileFailure{CompileFailure::Kind::TARGET_PREDICATE_MISMATCH};
            }
            expectations.push_back(predicate.expected_capability);
        }
        if (execution_targets != execution_covered || device_targets != device_covered) {
            return CompileFailure{CompileFailure::Kind::TARGET_PREDICATE_MISMATCH};
        }
        allocation_boundary.checkpoint(CompilerAllocationStage::CONSTRUCT_PROGRAM);
        return ValidatedRecoveryProgram(snapshot.recovery_id, snapshot.freeze_seq, snapshot.boot_id,
                                        snapshot.snapshot_digest, policy.digest, registry.version,
                                        registry.digest, snapshot.fault_batch, std::move(plans),
                                        std::move(coverage), std::move(derived.execution),
                                        std::move(derived.device), std::move(expectations),
                                        std::move(dag));
    } catch (...) {
        return CompileFailure{CompileFailure::Kind::ALLOCATION_FAILED};
    }
}

}  // namespace recovery
