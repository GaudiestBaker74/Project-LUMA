#pragma once
// =============================================================================
// Minimal self-contained test runner (zero dependencies).
//
//   TEST_CASE(name) { ... CHECK(expr); REQUIRE(expr); CHECK_EQ(a, b); ... }
//
// - CHECK: records a failure and continues.
// - REQUIRE: records a failure and returns from the test immediately.
// - CHECK_EQ: CHECK with value printing (requires operator<< for the type).
//
// The binary accepts:
//   (no args)              run all registered tests
//   --list-tests           print test names
//   --run-test NAME        run a single test
//   --filter SUBSTRING     only run tests containing SUBSTRING
// Exits 0 iff all run tests pass.
// =============================================================================

#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

namespace pc_test {

struct TestCase {
    const char* name;
    void (*fn)();
};

struct Failure {
    const char* file;
    int line;
    std::string message;
};

// Registry (ordered by registration = file order).
TestCase* begin();
TestCase* end();

int registerTest(const char* name, void (*fn)());

// Records a failure for the currently running test.
void recordFailure(const char* file, int line, const std::string& message);

// Result of running one test.
struct TestResult {
    bool passed;
    std::string name;
    std::vector<Failure> failures;
    int failuresReported = 0;
};

// Runs one test and returns the result.
TestResult runTest(const TestCase& test);

// Argument-parsing entry point (used by the test binary's main).
// Returns 0 on success, non-zero otherwise.
int runSuite(int argc, char** argv);

} // namespace pc_test

#define TEST_CASE(name)                                                            \
    static void pc_test_##name();                                                  \
    static const int pc_test_reg_##name = ::pc_test::registerTest(#name, &pc_test_##name); \
    static void pc_test_##name()

#define PC_TEST_STRINGIFY_IMPL(x) #x
#define PC_TEST_STRINGIFY(x) PC_TEST_STRINGIFY_IMPL(x)

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            ::pc_test::recordFailure(__FILE__, __LINE__, "CHECK failed: " #cond);        \
        }                                                                                \
    } while (0)

#define REQUIRE(cond)                                                                    \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            ::pc_test::recordFailure(__FILE__, __LINE__, "REQUIRE failed: " #cond);      \
            return;                                                                      \
        }                                                                                \
    } while (0)

#define CHECK_EQ(a, b)                                                                   \
    do {                                                                                 \
        const auto& pc_va = (a);                                                         \
        const auto& pc_vb = (b);                                                         \
        if (!(pc_va == pc_vb)) {                                                         \
            std::ostringstream pc_oss;                                                   \
            pc_oss << "CHECK_EQ(" #a ", " #b ") failed: [" << pc_va << "] != [" << pc_vb << "]"; \
            ::pc_test::recordFailure(__FILE__, __LINE__, pc_oss.str());                  \
        }                                                                                \
    } while (0)

// CHECK with explicit float tolerance.
#define CHECK_NEAR(a, b, eps)                                                            \
    do {                                                                                 \
        const double pc_a = static_cast<double>(a);                                      \
        const double pc_b = static_cast<double>(b);                                      \
        const double pc_eps = static_cast<double>(eps);                                  \
        if (!(pc_a > pc_b - pc_eps && pc_a < pc_b + pc_eps)) {                           \
            std::ostringstream pc_oss;                                                   \
            pc_oss << "CHECK_NEAR(" #a ", " #b ", " #eps ") failed: [" << pc_a << "] !~ [" << pc_b << "]"; \
            ::pc_test::recordFailure(__FILE__, __LINE__, pc_oss.str());                  \
        }                                                                                \
    } while (0)
