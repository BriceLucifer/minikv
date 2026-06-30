#include <httplib.h>

#include <gtest/gtest.h>

TEST(HttpDependencyTest, HeaderOnlyLibraryIsAvailable) {
  httplib::Headers headers{{"Content-Type", "text/plain"}};

  const auto it = headers.find("Content-Type");
  ASSERT_NE(it, headers.end());
  EXPECT_EQ(it->second, "text/plain");
}
