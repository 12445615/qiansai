#ifndef RECOVERY_CANONICAL_READER_HPP
#define RECOVERY_CANONICAL_READER_HPP

// Strict reader for bytes produced by CanonicalWriter. Every read either
// consumes exactly the field or fails; callers add the domain-specific
// structure and re-encode to enforce a single byte form.

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace recovery {

class CanonicalReader {
public:
    explicit CanonicalReader(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}
    [[nodiscard]] bool exhausted() const noexcept { return offset_ == bytes_.size(); }

    template <std::unsigned_integral Integer>
    bool read(Integer& value) noexcept {
        if (bytes_.size() - offset_ < sizeof(Integer)) return false;
        value = 0;
        for (std::size_t index = 0; index < sizeof(Integer); ++index)
            value |= static_cast<Integer>(Integer{bytes_[offset_ + index]} << (8U * index));
        offset_ += sizeof(Integer);
        return true;
    }
    bool read_count(std::size_t& count) noexcept {
        std::uint32_t value{};
        if (!read(value)) return false;
        count = value;
        return true;
    }
    // A fixed array as CanonicalWriter writes it: count prefix, then elements.
    template <std::size_t Size>
    bool read(std::array<std::uint8_t, Size>& value) noexcept {
        std::size_t count{};
        if (!read_count(count) || count != Size) return false;
        return read_raw(value.data(), Size);
    }
    // Raw bytes with no prefix (e.g. LinuxBootId).
    bool read_raw(std::uint8_t* out, std::size_t size) noexcept {
        if (bytes_.size() - offset_ < size) return false;
        for (std::size_t index = 0; index < size; ++index) out[index] = bytes_[offset_ + index];
        offset_ += size;
        return true;
    }
    bool read(std::vector<std::uint8_t>& value) {
        std::size_t count{};
        if (!read_count(count) || count > bytes_.size() - offset_) return false;
        value.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                     bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + count));
        offset_ += count;
        return true;
    }
    // std::optional tag: 0 = empty, 1 = present, anything else malformed.
    bool read_tag(bool& present) noexcept {
        std::uint8_t tag{};
        if (!read(tag) || tag > 1) return false;
        present = tag == 1;
        return true;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{};
};

}  // namespace recovery
#endif
