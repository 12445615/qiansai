#include "test_support.hpp"

#include <vector>

RECOVERY_TEST(recovery_runner_smoke) {
    RECOVERY_CHECK(true);
    RECOVERY_CHECK_EQ(20, 20);
}

RECOVERY_TEST(recovery_runner_equality_supports_non_streamable_types) {
    const std::vector<int> left{1, 2, 3};
    const std::vector<int> right{1, 2, 3};
    RECOVERY_CHECK_EQ(left, right);
}
