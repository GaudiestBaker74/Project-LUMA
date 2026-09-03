#include "tests/test_runner.h"

#include <cstdio>
#include <string>
#include <vector>

namespace pc_test {
namespace {

std::vector<TestCase>& registryStorage() {
    static std::vector<TestCase> tests;
    return tests;
}

std::vector<Failure>& failureStorage() {
    static std::vector<Failure> failures;
    return failures;
}

const char* kCurrentTestName = nullptr;
bool gSkipFlag = false;
const char* gSkipReason = nullptr;

} // namespace

TestCase* begin() {
    return registryStorage().data();
}

TestCase* end() {
    return registryStorage().data() + registryStorage().size();
}

int registerTest(const char* name, void (*fn)()) {
    registryStorage().push_back({name, fn});
    return static_cast<int>(registryStorage().size());
}

void recordFailure(const char* file, int line, const std::string& message) {
    failureStorage().push_back({file, line, message});
}

void markSkip(const char* reason) {
    gSkipFlag = true;
    gSkipReason = reason;
}

const char* skipReason() {
    return gSkipReason;
}

TestResult runTest(const TestCase& test) {
    failureStorage().clear();
    kCurrentTestName = test.name;
    gSkipFlag = false;
    gSkipReason = nullptr;

    test.fn();

    TestResult result;
    result.name = test.name;
    result.failures = failureStorage();
    result.failuresReported = static_cast<int>(failureStorage().size());
    if (gSkipFlag) {
        result.skipped = true;
        result.passed = true;
    } else {
        result.passed = failureStorage().empty();
    }
    return result;
}

} // namespace pc_test

namespace {

int runAll(const char* filter) {
    int passed = 0;
    int failed = 0;
    int skipped = 0;

    for (auto* it = pc_test::begin(); it != pc_test::end(); ++it) {
        const bool matches = !filter || std::string(it->name).find(filter) != std::string::npos;
        if (!matches) {
            ++skipped;
            continue;
        }
        // Flushed start marker: if a test aborts the process (OSPanic, AV),
        // the last [RUN ] line names the culprit even though the [ OK ]/[FAIL]
        // line never prints.
        std::printf("[RUN ] %s\n", it->name);
        std::fflush(stdout);
        const auto result = pc_test::runTest(*it);
        if (result.skipped) {
            std::printf("[SKIP] %s — %s\n", it->name, pc_test::skipReason() ? pc_test::skipReason() : "skipped");
            ++skipped;
        } else if (result.passed) {
            std::printf("[ OK ] %s\n", it->name);
            ++passed;
        } else {
            std::printf("[FAIL] %s\n", it->name);
            for (const auto& failure : result.failures) {
                std::printf("       %s:%d: %s\n", failure.file, failure.line, failure.message.c_str());
            }
            ++failed;
        }
    }

    std::printf("\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);
    return failed == 0 ? 0 : 1;
}

int runSingle(const char* name) {
    for (auto* it = pc_test::begin(); it != pc_test::end(); ++it) {
        if (std::string(it->name) == name) {
            std::printf("[RUN ] %s\n", it->name);
            std::fflush(stdout);
            const auto result = pc_test::runTest(*it);
            if (result.passed) {
                std::printf("[ OK ] %s\n", it->name);
                return 0;
            }
            std::printf("[FAIL] %s\n", it->name);
            for (const auto& failure : result.failures) {
                std::printf("       %s:%d: %s\n", failure.file, failure.line, failure.message.c_str());
            }
            return 1;
        }
    }
    std::printf("no such test: %s\n", name);
    return 2;
}

} // namespace

// Entry point used by the test binary main (see main_test.cpp). Parses the
// same arguments a standalone test runner would:
//   (no args)                run all
//   --list-tests
//   --run-test NAME
//   --filter SUBSTRING
namespace pc_test {
int runSuite(int argc, char** argv) {
    const char* filter = nullptr;
    if (argc >= 2) {
        const std::string arg = argv[1];
        if (arg == "--list-tests") {
            for (auto* it = pc_test::begin(); it != pc_test::end(); ++it) {
                std::printf("%s\n", it->name);
            }
            return 0;
        }
        if (arg == "--run-test" && argc >= 3) {
            return runSingle(argv[2]);
        }
        if (arg == "--filter" && argc >= 3) {
            filter = argv[2];
        } else if (arg[0] == '-') {
            std::printf("usage: galaxy-pc-tests [--list-tests] [--run-test NAME] [--filter SUBSTRING]\n");
            return 2;
        }
    }
    return runAll(filter);
}
} // namespace pc_test
