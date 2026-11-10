#include "test_support.hpp"

#include <iostream>
#include <string>

namespace {

int usage(const char* program) {
    std::cerr << "usage: " << program << " [--list] [--filter <substring>]\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    bool list_only = false;
    std::string filter;

    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument == "--list") {
            list_only = true;
        } else if (argument == "--filter") {
            if (++index == argc) {
                return usage(argv[0]);
            }
            filter = argv[index];
        } else {
            return usage(argv[0]);
        }
    }

    int failures = 0;
    for (const auto& test : recovery::test::registry()) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos) {
            continue;
        }
        if (list_only) {
            std::cout << test.name << '\n';
            continue;
        }

        try {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }

    return failures == 0 ? 0 : 1;
}
