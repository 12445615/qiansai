#include "recovery/crypto.hpp"

#include <openssl/evp.h>

#include <stdexcept>

namespace recovery {

Digest digest_bytes(const CanonicalBytes& bytes) {
    Digest digest{};
    unsigned int size = 0;
    const void* data = bytes.empty() ? nullptr : bytes.data();
    if (EVP_Digest(data, bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1
        || size != digest.size()) {
        throw std::runtime_error("OpenSSL SHA-256 digest failed");
    }
    return digest;
}

}  // namespace recovery
