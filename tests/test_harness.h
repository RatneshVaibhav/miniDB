#pragma once
// Tiny zero-dependency test harness. Register tests with TEST(group, name),
// assert with REQUIRE / REQUIRE_EQ, run them all from a main() that calls
// minidb::test::RunAll(argc, argv). Avoids pulling in GoogleTest (no network/cmake).
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace minidb::test {

struct TestCase {
  std::string group;
  std::string name;
  std::function<void()> fn;
};

inline std::vector<TestCase> &Registry() {
  static std::vector<TestCase> reg;
  return reg;
}

struct Registrar {
  Registrar(const char *group, const char *name, std::function<void()> fn) {
    Registry().push_back({group, name, std::move(fn)});
  }
};

// Thrown by failing assertions to abort the current test case only.
struct AssertionFailure {
  std::string msg;
};

inline void DoRequire(bool cond, const char *expr, const char *file, int line) {
  if (!cond) {
    std::ostringstream os;
    os << file << ":" << line << ": REQUIRE(" << expr << ") failed";
    throw AssertionFailure{os.str()};
  }
}

template <typename A, typename B>
void DoRequireEq(const A &a, const B &b, const char *ea, const char *eb,
                 const char *file, int line) {
  if (!(a == b)) {
    std::ostringstream os;
    os << file << ":" << line << ": REQUIRE_EQ(" << ea << ", " << eb
       << ") failed [" << a << " != " << b << "]";
    throw AssertionFailure{os.str()};
  }
}

inline int RunAll(int argc, char **argv) {
  std::string filter = (argc > 1) ? argv[1] : "";
  int passed = 0, failed = 0, skipped = 0;
  for (auto &tc : Registry()) {
    std::string full = tc.group + "." + tc.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      ++skipped;
      continue;
    }
    try {
      tc.fn();
      std::printf("  [ PASS ] %s\n", full.c_str());
      ++passed;
    } catch (const AssertionFailure &f) {
      std::printf("  [ FAIL ] %s\n           %s\n", full.c_str(), f.msg.c_str());
      ++failed;
    } catch (const std::exception &e) {
      std::printf("  [ FAIL ] %s\n           unexpected exception: %s\n", full.c_str(), e.what());
      ++failed;
    } catch (...) {
      std::printf("  [ FAIL ] %s\n           unknown exception\n", full.c_str());
      ++failed;
    }
  }
  std::printf("\n==== %d passed, %d failed, %d skipped ====\n", passed, failed, skipped);
  return failed == 0 ? 0 : 1;
}

}  // namespace minidb::test

#define TEST(group, name)                                                       \
  static void test_##group##_##name();                                          \
  static ::minidb::test::Registrar reg_##group##_##name(#group, #name,          \
                                                        test_##group##_##name); \
  static void test_##group##_##name()

#define REQUIRE(cond) ::minidb::test::DoRequire((cond), #cond, __FILE__, __LINE__)
#define REQUIRE_EQ(a, b) ::minidb::test::DoRequireEq((a), (b), #a, #b, __FILE__, __LINE__)
#define REQUIRE_THROWS(stmt)                                       \
  do {                                                             \
    bool threw = false;                                            \
    try { stmt; } catch (...) { threw = true; }                    \
    ::minidb::test::DoRequire(threw, "expected throw: " #stmt, __FILE__, __LINE__); \
  } while (0)
