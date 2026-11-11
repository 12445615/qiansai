#ifndef RECOVERY_CANONICAL_HPP
#define RECOVERY_CANONICAL_HPP

#include "recovery/types.hpp"

#include <array>
#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace recovery {

struct FrozenRuntimeSnapshot;

namespace detail {
template <std::size_t Count>
constexpr void validate_variant_alternative_count() {
    static_assert(Count <= 256, "canonical variant index exceeds one byte");
}
}  // namespace detail

using CanonicalBytes = std::vector<std::uint8_t>;

class CanonicalWriter {
public:
    void write_byte(std::uint8_t value);
    void write_bytes(const std::uint8_t* data, std::size_t size);
    void write_count(std::size_t count);
    [[nodiscard]] CanonicalBytes finish() &&;

private:
    CanonicalBytes bytes_;
};

template <typename Value>
CanonicalBytes canonical_bytes(const Value& value);

template <std::unsigned_integral Integer>
void canonical_write(CanonicalWriter& writer, Integer value) {
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        writer.write_byte(static_cast<std::uint8_t>(value & 0xffU));
        value >>= 8U;
    }
}

template <std::signed_integral Integer>
void canonical_write(CanonicalWriter& writer, Integer value) {
    canonical_write(writer, static_cast<std::make_unsigned_t<Integer>>(value));
}

inline void canonical_write(CanonicalWriter& writer, bool value) {
    writer.write_byte(value ? 1 : 0);
}

template <typename Enum>
    requires std::is_enum_v<Enum>
void canonical_write(CanonicalWriter& writer, Enum value) {
    canonical_write(writer, static_cast<std::underlying_type_t<Enum>>(value));
}

inline void canonical_write(CanonicalWriter& writer, const std::string& value) {
    writer.write_count(value.size());
    writer.write_bytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
}

template <typename First, typename Second>
void canonical_write(CanonicalWriter& writer, const std::pair<First, Second>& value) {
    canonical_write(writer, value.first);
    canonical_write(writer, value.second);
}

template <typename Value>
void canonical_write(CanonicalWriter& writer, const std::vector<Value>& values) {
    writer.write_count(values.size());
    for (const auto& value : values) {
        canonical_write(writer, value);
    }
}

template <typename Value, std::size_t Size>
void canonical_write(CanonicalWriter& writer, const std::array<Value, Size>& values) {
    writer.write_count(Size);
    for (const auto& value : values) {
        canonical_write(writer, value);
    }
}

template <typename Value>
void canonical_write(CanonicalWriter& writer, const std::optional<Value>& value) {
    writer.write_byte(value.has_value() ? 1 : 0);
    if (value) {
        canonical_write(writer, *value);
    }
}

template <typename... Alternatives>
void canonical_write(CanonicalWriter& writer, const std::variant<Alternatives...>& value) {
    detail::validate_variant_alternative_count<sizeof...(Alternatives)>();
    if (value.valueless_by_exception()) {
        throw std::invalid_argument("canonical variant is valueless_by_exception");
    }
    writer.write_byte(static_cast<std::uint8_t>(value.index()));
    std::visit([&writer](const auto& alternative) { canonical_write(writer, alternative); }, value);
}

template <typename Key, typename Value, typename Compare, typename Allocator>
void canonical_write(CanonicalWriter& writer,
                     const std::map<Key, Value, Compare, Allocator>& values) {
    writer.write_count(values.size());
    std::vector<std::pair<CanonicalBytes, CanonicalBytes>> encoded;
    encoded.reserve(values.size());
    for (const auto& [key, value] : values) {
        encoded.emplace_back(canonical_bytes(key), canonical_bytes(value));
    }
    std::sort(encoded.begin(), encoded.end());
    for (const auto& [key, value] : encoded) {
        writer.write_bytes(key.data(), key.size());
        writer.write_bytes(value.data(), value.size());
    }
}

template <typename Value, typename Compare, typename Allocator>
void canonical_write(CanonicalWriter& writer,
                     const std::set<Value, Compare, Allocator>& values) {
    writer.write_count(values.size());
    std::vector<CanonicalBytes> encoded;
    encoded.reserve(values.size());
    for (const auto& value : values) {
        encoded.push_back(canonical_bytes(value));
    }
    std::sort(encoded.begin(), encoded.end());
    for (const auto& value : encoded) {
        writer.write_bytes(value.data(), value.size());
    }
}

inline void canonical_write(CanonicalWriter& writer, const ProcessIdentity& value) {
    canonical_write(writer, value.pid);
    canonical_write(writer, value.start_generation);
}

void canonical_write(CanonicalWriter&, const RecoveryInstanceId&);
void canonical_write(CanonicalWriter&, const ProcessExitBindingId&);
void canonical_write(CanonicalWriter&, const ProcessExitBindingDescriptor&);
void canonical_write(CanonicalWriter&, const FrozenRuntimeSnapshot&);

inline void canonical_write(CanonicalWriter& writer, const ProcessPrincipal& value) {
    canonical_write(writer, value.process);
}

inline void canonical_write(CanonicalWriter&, const CoordinatorPrincipal&) {}

template <typename Value>
CanonicalBytes canonical_bytes(const Value& value) {
    CanonicalWriter writer;
    canonical_write(writer, value);
    return std::move(writer).finish();
}

}  // namespace recovery

#endif
