#include "recovery/program.hpp"

int main() {
    recovery::ValidatedRecoveryProgram program{};
    recovery::ValidatedRecoveryProgram full(
        {}, 0, 0, {}, {}, 0, {}, {}, {}, {}, {}, {}, {}, {});
    (void)program;
    (void)full;
}
