#ifndef RECOVERY_CRYPTO_HPP
#define RECOVERY_CRYPTO_HPP

#include "recovery/canonical.hpp"

namespace recovery {

[[nodiscard]] Digest digest_bytes(const CanonicalBytes& bytes);

template <typename Value>
[[nodiscard]] Digest canonical_digest(const Value& value) {
    return digest_bytes(canonical_bytes(value));
}

}  // namespace recovery

#endif
