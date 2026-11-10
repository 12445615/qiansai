#include "recovery/canonical.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace recovery {

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
