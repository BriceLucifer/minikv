#include <httplib.h>
#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

TEST(HttpDependencyTest, HeaderOnlyLibraryIsAvailable) {
  httplib::Headers headers{{"Content-Type", "text/plain"}};

  const auto it = headers.find("Content-Type");
  ASSERT_NE(it, headers.end());
  EXPECT_EQ(it->second, "text/plain");
}

TEST(JsonDependencyTest, HeaderOnlyLibraryIsAvailable) {
  const auto body = nlohmann::json{{"name", "minikv"}, {"replicas", 3}};

  EXPECT_EQ(body.at("name"), "minikv");
  EXPECT_EQ(body.at("replicas"), 3);
}
