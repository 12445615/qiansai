// EXPECT-DIAG: use of deleted function
#include "recovery/compiler.hpp"

void forbidden(recovery::CompileResult left, recovery::CompileResult right) {
    auto& a = std::get<recovery::ValidatedRecoveryProgram>(left);
    auto& b = std::get<recovery::ValidatedRecoveryProgram>(right);
    a = std::move(b);
}
