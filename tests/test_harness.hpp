#pragma once

#include <cstdio>
#include <cstdlib>

namespace omnibuf_test {

inline int g_passed = 0;
inline int g_failed = 0;
inline int g_case_failures = 0;

}  // namespace omnibuf_test

#define EXPECT(cond)                                                               \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      std::fprintf(stderr, "  FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond);     \
      ++::omnibuf_test::g_case_failures;                                           \
    }                                                                              \
  } while (0)

#define EXPECT_MSG(cond, msg)                                                      \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      std::fprintf(stderr, "  FAIL [%s:%d]: %s (%s)\n", __FILE__, __LINE__, #cond, \
                   (msg));                                                         \
      ++::omnibuf_test::g_case_failures;                                           \
    }                                                                              \
  } while (0)

#define RUN_CASE(fn)                                 \
  do {                                               \
    ::omnibuf_test::g_case_failures = 0;             \
    fn();                                            \
    if (::omnibuf_test::g_case_failures == 0) {      \
      ++::omnibuf_test::g_passed;                    \
      std::printf("PASS %s\n", #fn);                 \
    } else {                                         \
      ++::omnibuf_test::g_failed;                    \
      std::printf("FAIL %s\n", #fn);                 \
    }                                                \
  } while (0)
