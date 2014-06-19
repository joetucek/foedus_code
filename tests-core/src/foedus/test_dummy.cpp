/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#include <gtest/gtest.h>
#include <cstdlib>
namespace foedus {
DEFINE_TEST_CASE_PACKAGE(DummyTest, foedus);
/**
 * Just to see if Jenkins can pick up aborted testcases.
 * This is a bit trickier than it should be.
 * I'm not sure if I should blame ctest, jenkins, or gtest (or all of them).
 * Related URLs:
 *   https://groups.google.com/forum/#!topic/googletestframework/NK5cAEqsioY
 *   https://code.google.com/p/googletest/issues/detail?id=342
 *   https://code.google.com/p/googletest/issues/detail?id=311
 */
TEST(DummyTest, Abort) {
  // Disabled usually. Enable only when to test Jenkins.
  // std::abort();
}
TEST(DummyTest, NotAbort) {
}

}  // namespace foedus
