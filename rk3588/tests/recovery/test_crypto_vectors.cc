#include "recovery/crypto.hpp"
#include "test_support.hpp"

#include <string>

namespace {

std::string hex(const recovery::Digest& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0f]);
    }
    return out;
}

RECOVERY_TEST(sha256_empty_golden_vector) {
    RECOVERY_CHECK_EQ(hex(recovery::digest_bytes({})),
                      std::string("e3b0c44298fc1c149afbf4c8996fb924"
                                  "27ae41e4649b934ca495991b7852b855"));
}

RECOVERY_TEST(sha256_abc_golden_vector) {
    const recovery::CanonicalBytes bytes{'a', 'b', 'c'};
    RECOVERY_CHECK_EQ(hex(recovery::digest_bytes(bytes)),
                      std::string("ba7816bf8f01cfea414140de5dae2223"
                                  "b00361a396177a9cb410ff61f20015ad"));
}

RECOVERY_TEST(canonical_digest_has_independent_fixed_vector) {
    const auto digest = recovery::canonical_digest(std::uint64_t{0x0102});
    RECOVERY_CHECK_EQ(hex(digest),
                      std::string("fb59317b8103f19f3850f0be3fe62db1"
                                  "aa481a9a7b31c590e6d243e98eacd375"));
}

}  // namespace
