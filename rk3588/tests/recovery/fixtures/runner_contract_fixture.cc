#include "test_support.hpp"

#include <vector>

RECOVERY_TEST(runner_contract_pass) {
    RECOVERY_CHECK(true);
}

RECOVERY_TEST(runner_contract_other_pass) {
    RECOVERY_CHECK(true);
}

RECOVERY_TEST(runner_contract_equality_failure) {
    RECOVERY_CHECK_EQ(1, 2);
}

RECOVERY_TEST(runner_contract_nonstreamable_pass) {
    RECOVERY_CHECK_EQ(std::vector<int>({1, 2}), std::vector<int>({1, 2}));
}

RECOVERY_TEST(runner_contract_vector_equality_failure) {
    RECOVERY_CHECK_EQ(std::vector<int>({1, 2}), std::vector<int>({2, 1}));
}
