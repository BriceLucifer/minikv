#include "placement.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

TEST(PlacementTest, Key2PathMatchesGoImplementation) {
  EXPECT_EQ(minikv::placement::key2path("hello"), "/5d/41/aGVsbG8=");
  EXPECT_EQ(minikv::placement::key2path("helloworld"),
            "/fc/5e/aGVsbG93b3JsZA==");
}

TEST(PlacementTest, Key2VolumeMatchesGoImplementation) {
  const auto volumes = std::vector<std::string>{"larry", "moe", "curly"};

  auto volume_name = [](const std::string &volume) {
    return volume.substr(0, volume.find('/'));
  };

  EXPECT_EQ(volume_name(minikv::placement::key2volume("hello", volumes, 1, 3)[0]),
            "larry");
  EXPECT_EQ(volume_name(
                minikv::placement::key2volume("helloworld", volumes, 1, 3)[0]),
            "curly");
  EXPECT_EQ(volume_name(minikv::placement::key2volume("world", volumes, 1, 3)[0]),
            "moe");
  EXPECT_EQ(volume_name(minikv::placement::key2volume("blah", volumes, 1, 3)[0]),
            "curly");
}

TEST(PlacementTest, Key2VolumeOmitsSubvolumeWhenCountIsOne) {
  const auto volumes = std::vector<std::string>{"larry", "moe", "curly"};

  EXPECT_EQ(minikv::placement::key2volume("hello", volumes, 1, 1),
            std::vector<std::string>{"larry"});
}

TEST(PlacementTest, Key2VolumeRejectsInvalidConfiguration) {
  const auto volumes = std::vector<std::string>{"larry", "moe", "curly"};

  EXPECT_THROW(minikv::placement::key2volume("hello", volumes, -1, 1),
               std::invalid_argument);
  EXPECT_THROW(minikv::placement::key2volume("hello", volumes, 4, 1),
               std::invalid_argument);
  EXPECT_THROW(minikv::placement::key2volume("hello", volumes, 1, 0),
               std::invalid_argument);
}

TEST(PlacementTest, NeedsRebalanceComparesVolumeOrder) {
  EXPECT_FALSE(minikv::placement::needs_rebalance({"a", "b"}, {"a", "b"}));
  EXPECT_TRUE(minikv::placement::needs_rebalance({"a", "b"}, {"b", "a"}));
  EXPECT_TRUE(minikv::placement::needs_rebalance({"a"}, {"a", "b"}));
}
