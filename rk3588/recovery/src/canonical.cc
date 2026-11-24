#include "recovery/canonical.hpp"
#include "recovery/program.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace recovery {

void canonical_write(CanonicalWriter& writer, const RecoveryInstanceId& value) {
    canonical_write(writer, value.boot_id);
    canonical_write(writer, value.recovery_seq);
    canonical_write(writer, value.nonce);
}

void canonical_write(CanonicalWriter& writer, const GrantIdentity& value) {
    canonical_write(writer, value.boot_id);
    canonical_write(writer, value.admission_generation);
    canonical_write(writer, value.seq);
}

void canonical_write(CanonicalWriter& writer, const ProcessExitBindingId& value) {
    canonical_write(writer, value.recovery_id);
    canonical_write(writer, value.freeze_seq);
    canonical_write(writer, value.binding_seq);
}

void canonical_write(CanonicalWriter& writer, const ProcessExitBindingDescriptor& value) {
    canonical_write(writer, value.id);
    canonical_write(writer, value.process);
}

void canonical_write(CanonicalWriter& writer, const FrozenRuntimeSnapshot& value) {
    canonical_write(writer, value.recovery_id);
    canonical_write(writer, value.freeze_seq);
    canonical_write(writer, value.boot_id);
    canonical_write(writer, value.closing_generation);
    canonical_write(writer, value.grant_cutoff);
    canonical_write(writer, value.ledger_prefix);
    canonical_write(writer, value.ledger_prefix_digest);
    canonical_write(writer, value.owner_ack_canonical_digest);
    canonical_write(writer, value.fault_batch);
    canonical_write(writer, value.fault_batch_cutoff);
    canonical_write(writer, value.fault_batch_digest);
    canonical_write(writer, value.owner_executor_bindings);
    canonical_write(writer, value.process_exit_bindings);
    canonical_write(writer, value.epoch_snapshot);
    canonical_write(writer, value.snapshot_digest);
}

void CanonicalWriter::write_byte(std::uint8_t value) {
    bytes_.push_back(value);
}

void CanonicalWriter::write_bytes(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return;
    }
    if (data == nullptr) {
        throw std::invalid_argument("non-empty canonical bytes require non-null data");
    }
    bytes_.insert(bytes_.end(), data, data + size);
}

void CanonicalWriter::write_count(std::size_t count) {
    if (count > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("canonical container exceeds uint32 length");
    }
    canonical_write(*this, static_cast<std::uint32_t>(count));
}

CanonicalBytes CanonicalWriter::finish() && {
    return std::move(bytes_);
}

}  // namespace recovery
