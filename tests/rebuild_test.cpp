#include "rebuild.hpp"

#include "base64.hpp"
#include "index.hpp"
#include "placement.hpp"
#include "record.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path testDbPath(std::string_view name) {
  auto path = std::filesystem::temp_directory_path();
  path /= "minikv-rebuild-test";
  path /= name;
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path;
}

} // namespace

TEST(RebuildTest, ParsesNginxAutoindexDirectoryListing) {
  const auto entries = minikv::rebuild::parseDirectoryListing(
      R"([{"name":"00","type":"directory","mtime":"ignored"},)"
      R"({"name":"aGVsbG8=","type":"file","mtime":"ignored"}])");

  ASSERT_EQ(entries.size(), 2U);
  EXPECT_EQ(entries[0].name, "00");
  EXPECT_EQ(entries[0].type, "directory");
  EXPECT_EQ(entries[1].name, "aGVsbG8=");
  EXPECT_EQ(entries[1].type, "file");
}

TEST(RebuildTest, ClassifiesHashAndSubvolumeDirectoriesLikeGo) {
  EXPECT_TRUE(minikv::rebuild::isHashDirectory({"0f", "directory", ""}));
  EXPECT_TRUE(minikv::rebuild::isHashDirectory({"A9", "directory", ""}));
  EXPECT_FALSE(minikv::rebuild::isHashDirectory({"sv00", "directory", ""}));
  EXPECT_FALSE(minikv::rebuild::isHashDirectory({"0g", "directory", ""}));
  EXPECT_FALSE(minikv::rebuild::isHashDirectory({"0f", "file", ""}));

  EXPECT_TRUE(minikv::rebuild::isSubvolumeDirectory({"sv00", "directory", ""}));
  EXPECT_TRUE(minikv::rebuild::isSubvolumeDirectory({"sv0F", "directory", ""}));
  EXPECT_FALSE(minikv::rebuild::isSubvolumeDirectory({"0f", "directory", ""}));
  EXPECT_FALSE(minikv::rebuild::isSubvolumeDirectory({"sv00", "file", ""}));
}

TEST(RebuildTest, OrdersRebuiltVolumesByPreferredOrderThenExtras) {
  const auto ordered = minikv::rebuild::orderedRebuiltVolumes(
      {"old-volume", "volume-b", "volume-a"},
      {"volume-a", "volume-b", "volume-c"});

  EXPECT_EQ(ordered,
            (std::vector<std::string>{"volume-a", "volume-b", "old-volume"}));
}

TEST(RebuildTest, RebuildObjectDecodesKeyAndMergesVolumes) {
  const auto path = testDbPath("object");
  const auto cleanup = [&] { std::filesystem::remove_all(path); };

  {
    auto index = minikv::index::LevelDbIndex{path};
    const auto options = minikv::rebuild::Options{
        .db_path = path,
        .volumes = {"volume-a", "volume-b", "volume-c"},
        .replicas = 2,
        .subvolumes = 1,
    };
    const auto key = std::string{"/hello"};
    const auto preferred = minikv::placement::key2volume(
        key, options.volumes, options.replicas, options.subvolumes);

    ASSERT_TRUE(minikv::rebuild::rebuildObject(
        index, options, preferred[1], minikv::base64_encode(key)));
    ASSERT_TRUE(minikv::rebuild::rebuildObject(
        index, options, preferred[0], minikv::base64_encode(key)));
    ASSERT_TRUE(minikv::rebuild::rebuildObject(
        index, options, "old-volume", minikv::base64_encode(key)));

    const auto rec = index.getRecord(key);
    EXPECT_EQ(rec.deleted, minikv::record::Deleted::NO);
    EXPECT_EQ(rec.hash, "");
    EXPECT_EQ(rec.rvolumes,
              (std::vector<std::string>{
                  preferred[0],
                  preferred[1],
                  "old-volume",
              }));
  }

  cleanup();
}

TEST(RebuildTest, RebuildObjectRejectsInvalidBase64Name) {
  const auto path = testDbPath("invalid-base64");
  const auto cleanup = [&] { std::filesystem::remove_all(path); };

  {
    auto index = minikv::index::LevelDbIndex{path};
    const auto options = minikv::rebuild::Options{
        .db_path = path,
        .volumes = {"volume-a"},
        .replicas = 1,
        .subvolumes = 1,
    };

    EXPECT_FALSE(minikv::rebuild::rebuildObject(index, options, "volume-a",
                                                "not-valid!"));
    EXPECT_EQ(index.listRecords("/", "", 0).records.size(), 0U);
  }

  cleanup();
}
