#ifndef RECOVERY_TEST_SUPPORT_HPP
#define RECOVERY_TEST_SUPPORT_HPP

#include "recovery/program.hpp"

#include <exception>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef RECOVERY_TESTING
namespace recovery {

struct RecoveryTestAccess {
    static FaultFact make_fault(BootId boot, std::uint64_t sequence,
                                const FaultReport& report) {
        return {FaultIdentity{boot, sequence}, report.kind, report.subject,
                report.observed_epochs, report.evidence};
    }
    static void clear_fault_batch(RecoveryCoordinator&);
    static void set_fault_batch(RecoveryCoordinator&, std::vector<FaultFact>);
    static void set_fault_seq_counter(RecoveryCoordinator&, std::uint64_t);
    static std::uint64_t fault_seq_counter(const RecoveryCoordinator&);
};

}  // namespace recovery
#endif

namespace recovery::test {

template <typename Value>
concept StreamInsertable = requires(std::ostream& output, const Value& value) {
    output << value;
};

using TestFunction = void (*)();

struct TestCase {
    std::string name;
    TestFunction function;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

class Registrar {
public:
    Registrar(const char* name, TestFunction function) {
        registry().push_back(TestCase{name, function});
    }
};

class Failure final : public std::exception {
public:
    explicit Failure(std::string message) : message_(std::move(message)) {}

    const char* what() const noexcept override { return message_.c_str(); }

private:
    std::string message_;
};

[[noreturn]] inline void fail(const char* file, int line, const std::string& message) {
    std::ostringstream output;
    output << file << ':' << line << ": " << message;
    throw Failure(output.str());
}

template <typename Left, typename Right>
void check_equal(const Left& left,
                 const Right& right,
                 const char* left_expression,
                 const char* right_expression,
                 const char* file,
                 int line) {
    if (!(left == right)) {
        std::ostringstream message;
        message << "expected equality: " << left_expression << " == " << right_expression;
        if constexpr (StreamInsertable<Left> && StreamInsertable<Right>) {
            message << " (actual " << left << " vs " << right << ')';
        }
        fail(file, line, message.str());
    }
}

}  // namespace recovery::test

#define RECOVERY_TEST_CONCAT_INNER(left, right) left##right
#define RECOVERY_TEST_CONCAT(left, right) RECOVERY_TEST_CONCAT_INNER(left, right)

#define RECOVERY_TEST(name)                                                        \
    static void name();                                                            \
    static const ::recovery::test::Registrar RECOVERY_TEST_CONCAT(                \
        recovery_test_registrar_, __LINE__)(#name, &name);                         \
    static void name()

#define RECOVERY_CHECK(expression)                                                 \
    do {                                                                           \
        if (!(expression)) {                                                       \
            ::recovery::test::fail(__FILE__, __LINE__,                             \
                                   "check failed: " #expression);                 \
        }                                                                          \
    } while (false)

#define RECOVERY_CHECK_EQ(left, right)                                             \
    do {                                                                           \
        const auto& recovery_test_left = (left);                                    \
        const auto& recovery_test_right = (right);                                  \
        ::recovery::test::check_equal(recovery_test_left, recovery_test_right,      \
                                      #left, #right, __FILE__, __LINE__);           \
    } while (false)

#endif
