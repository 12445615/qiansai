// G19: LinuxBootId is a full 16-byte UUID in type, parse and persisted
// encoding; keys from UUIDs that share either half never collide.
#include "recovery/ingress.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <fstream>
#include <string>

namespace {
static_assert(sizeof(recovery::LinuxBootId) == 16);

recovery::LinuxBootId parsed(const char* text) {
    const auto id = recovery::parse_linux_boot_id(text);
    RECOVERY_CHECK(id.has_value());
    return *id;
}

recovery::DedupKey key_for(const recovery::LinuxBootId& boot) {
    return {{1}, {boot, {7}, {4242, 99}}, {}};
}
}  // namespace

RECOVERY_TEST(G19_parse_accepts_only_the_canonical_8_4_4_4_12_form) {
    const auto id = parsed("6f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0\n");
    const std::array<std::uint8_t, 16> expected{0x6f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78,
                                                0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0};
    RECOVERY_CHECK(id.bytes == expected);
    RECOVERY_CHECK(parsed("6F1E2D3C-4B5A-6978-8796-A5B4C3D2E1F0") == id);
    for (const char* bad : {"6f1e2d3c4b5a69788796a5b4c3d2e1f0",
                            "6f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f",
                            "6f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f00",
                            "6f1e2d3c4-b5a-6978-8796-a5b4c3d2e1f0",
                            "6f1e2d3c-4b5a-6978-8796-a5b4c3d2e1fg",
                            "6f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0\n\n"}) {
        RECOVERY_CHECK(!recovery::parse_linux_boot_id(bad).has_value());
    }
}

// Captured from the running kernel rather than composed by hand.
RECOVERY_TEST(G19_real_proc_boot_id_round_trips_through_the_parser) {
    std::ifstream file{"/proc/sys/kernel/random/boot_id"};
    std::string text;
    RECOVERY_CHECK(static_cast<bool>(std::getline(file, text)));
    const auto id = recovery::read_linux_boot_id();
    RECOVERY_CHECK(id.has_value());
    RECOVERY_CHECK(recovery::parse_linux_boot_id(text) == id);
    std::string hex;
    static constexpr char kDigits[] = "0123456789abcdef";
    for (const auto byte : id->bytes) {
        hex.push_back(kDigits[byte >> 4]);
        hex.push_back(kDigits[byte & 0xf]);
    }
    std::string stripped = text;
    stripped.erase(std::remove(stripped.begin(), stripped.end(), '-'), stripped.end());
    std::transform(stripped.begin(), stripped.end(), stripped.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    RECOVERY_CHECK_EQ(hex, stripped);
}

RECOVERY_TEST(G19_persisted_encoding_is_exactly_the_16_uuid_bytes) {
    const auto id = parsed("6f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0");
    const auto bytes = recovery::canonical_bytes(id);
    RECOVERY_CHECK_EQ(bytes.size(), 16U);
    RECOVERY_CHECK(std::equal(bytes.begin(), bytes.end(), id.bytes.begin()));
}

// ② both halves matter: A shares the high 8 bytes, B shares the low 8 bytes.
RECOVERY_TEST(G19_ids_sharing_either_half_give_distinct_keys) {
    const auto a1 = parsed("11111111-2222-3333-4444-555555555555");
    const auto a2 = parsed("11111111-2222-3333-4444-555555555556");
    const auto b1 = parsed("01111111-2222-3333-4444-555555555555");
    const auto b2 = parsed("11111111-2222-3334-4444-555555555555");
    for (const auto& [left, right] : {std::pair{a1, a2}, std::pair{a1, b1}, std::pair{a1, b2},
                                      std::pair{b1, b2}}) {
        RECOVERY_CHECK(left != right);
        RECOVERY_CHECK(key_for(left) != key_for(right));
        RECOVERY_CHECK(recovery::canonical_bytes(key_for(left))
                       != recovery::canonical_bytes(key_for(right)));
    }
}
