#include "test_support.hpp"

#include "recovery/canonical.hpp"
#include "recovery/types.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace recovery;

namespace canonical_test {
struct ThrowOnMove {
    ThrowOnMove() = default;
    ThrowOnMove(const ThrowOnMove&) = delete;
    ThrowOnMove(ThrowOnMove&&) { throw std::runtime_error("expected move failure"); }
};
inline void canonical_write(recovery::CanonicalWriter&, const ThrowOnMove&) {}
}  // namespace canonical_test

RECOVERY_TEST(R42_canonical_nested_lengths_prevent_concatenation_collision) {
    const auto left = canonical_bytes(std::vector<std::string>{"ab", "c"});
    const auto right = canonical_bytes(std::vector<std::string>{"a", "bc"});
    RECOVERY_CHECK(left != right);
}

RECOVERY_TEST(canonical_integers_are_fixed_width_little_endian) {
    RECOVERY_CHECK_EQ(canonical_bytes(std::uint32_t{0x78563412}),
                      (CanonicalBytes{0x12, 0x34, 0x56, 0x78}));
}

RECOVERY_TEST(canonical_signed_integers_use_twos_complement_little_endian) {
    RECOVERY_CHECK_EQ(canonical_bytes(std::int64_t{-2}),
                      (CanonicalBytes{0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}));
}

RECOVERY_TEST(canonical_containers_are_length_delimited) {
    RECOVERY_CHECK_EQ(canonical_bytes(std::string{"ab"}),
                      (CanonicalBytes{2, 0, 0, 0, 'a', 'b'}));
}

RECOVERY_TEST(canonical_map_encoding_has_deterministic_key_order) {
    std::map<std::uint32_t, std::string> first{{2, "b"}, {1, "a"}};
    std::map<std::uint32_t, std::string> second{{1, "a"}, {2, "b"}};
    RECOVERY_CHECK_EQ(canonical_bytes(first), canonical_bytes(second));
}

RECOVERY_TEST(canonical_set_encoding_has_deterministic_value_order) {
    RECOVERY_CHECK_EQ(canonical_bytes(std::set<std::uint32_t>{3, 1, 2}),
                      canonical_bytes(std::set<std::uint32_t>{1, 2, 3}));
}

RECOVERY_TEST(canonical_set_order_is_independent_of_comparator) {
    RECOVERY_CHECK_EQ(canonical_bytes(std::set<std::uint32_t, std::less<>>{1, 2, 3}),
                      canonical_bytes(std::set<std::uint32_t, std::greater<>>{1, 2, 3}));
}

RECOVERY_TEST(canonical_map_order_is_independent_of_comparator) {
    const std::map<std::uint32_t, std::string, std::less<>> ascending{{1, "a"}, {2, "b"}};
    const std::map<std::uint32_t, std::string, std::greater<>> descending{{1, "a"}, {2, "b"}};
    RECOVERY_CHECK_EQ(canonical_bytes(ascending), canonical_bytes(descending));
}

RECOVERY_TEST(canonical_bool_and_enum_have_pinned_widths) {
    RECOVERY_CHECK_EQ(canonical_bytes(true), (CanonicalBytes{1}));
    RECOVERY_CHECK_EQ(canonical_bytes(ExecutionProofKind::PROCESS_EXIT_CONFIRMED),
                      (CanonicalBytes{1, 0, 0, 0}));
}

RECOVERY_TEST(canonical_writer_null_zero_is_safe_and_null_nonzero_is_rejected) {
    CanonicalWriter writer;
    writer.write_bytes(nullptr, 0);
    RECOVERY_CHECK(std::move(writer).finish().empty());
    bool rejected = false;
    try {
        CanonicalWriter invalid;
        invalid.write_bytes(nullptr, 1);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RECOVERY_CHECK(rejected);
}

RECOVERY_TEST(canonical_variant_rejects_valueless_by_exception) {
    std::variant<std::uint32_t, canonical_test::ThrowOnMove> value(std::in_place_index<0>, 1);
    try {
        value.emplace<1>(canonical_test::ThrowOnMove{});
    } catch (const std::runtime_error&) {
    }
    RECOVERY_CHECK(value.valueless_by_exception());
    bool rejected = false;
    try {
        (void)canonical_bytes(value);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RECOVERY_CHECK(rejected);
}

RECOVERY_TEST(canonical_principal_variant_starts_with_one_byte_index) {
    const auto process = canonical_bytes(
        ExecutorPrincipal{ProcessPrincipal{ProcessIdentity{1, 2}}});
    const auto coordinator = canonical_bytes(ExecutorPrincipal{CoordinatorPrincipal{}});
    RECOVERY_CHECK(process.front() == 0);
    RECOVERY_CHECK(coordinator.front() == 1);
    RECOVERY_CHECK(process != coordinator);
}
