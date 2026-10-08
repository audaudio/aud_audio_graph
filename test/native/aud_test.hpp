// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// A tiny test harness for the native tests: AUD_TEST registers a test,
// AUD_CHECK records a failure with its location, the runner prints the
// summary and returns the failure count.

#ifndef AUD_TEST_HPP
#define AUD_TEST_HPP

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace aud_test {

struct Test {
  std::string name;
  std::function<void()> body;
};

inline std::vector<Test>& tests() {
  static std::vector<Test> list;
  return list;
}

inline int& failures() {
  static int count = 0;
  return count;
}

inline const char*& currentTest() {
  static const char* name = "";
  return name;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> body) {
    tests().push_back({name, std::move(body)});
  }
};

inline void fail(const char* expression, const char* file, int line) {
  failures() += 1;
  std::printf("  FAILED %s:%d in %s: %s\n", file, line, currentTest(),
              expression);
}

inline bool near(double a, double b, double tolerance, const char* file,
                 int line) {
  if (std::fabs(a - b) <= tolerance) return true;
  std::printf("  %s:%d: %.9g is not within %g of %.9g\n", file, line, a,
              tolerance, b);
  return false;
}

inline int run() {
  int passed = 0;
  for (const Test& test : tests()) {
    const int before = failures();
    currentTest() = test.name.c_str();
    test.body();
    if (failures() == before) {
      passed += 1;
    } else {
      std::printf("FAILED: %s\n", test.name.c_str());
    }
  }
  std::printf("%d of %zu native tests passed, %d checks failed\n", passed,
              tests().size(), failures());
  return failures() == 0 ? 0 : 1;
}

}  // namespace aud_test

#define AUD_TEST(name)                                             \
  static void aud_test_##name();                                   \
  static aud_test::Registrar aud_registrar_##name(#name,           \
                                                  aud_test_##name); \
  static void aud_test_##name()

#define AUD_CHECK(expression)                                   \
  do {                                                          \
    if (!(expression)) aud_test::fail(#expression, __FILE__, __LINE__); \
  } while (0)

#define AUD_CHECK_NEAR(a, b, tolerance) \
  AUD_CHECK(aud_test::near((a), (b), (tolerance), __FILE__, __LINE__))

#endif  // AUD_TEST_HPP
