// EXPECT-DIAG: passing.*const.*discards qualifiers
#include "recovery/program.hpp"

int main() {
    recovery::ValidatedRecoveryProgram& program = *static_cast<recovery::ValidatedRecoveryProgram*>(nullptr);
    program.action_dag().clear();
}
