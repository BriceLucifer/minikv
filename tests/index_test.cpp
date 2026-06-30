#include "index.hpp"
#include "record.hpp"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

std::filesystem::path testDbPath(std::string_view name) {
  auto path = std::filesystem::temp_directory_path();
  path /= "minikv-index-test";
  path /= name;
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path;
}

} // namespace

TEST(IndexTest, MissingKeyReturnsHardDeletedRecord) {
  const auto path = testDbPath("missing-key");
  const auto cleanup = [&] { std::filesystem::remove_all(path); };

  {
    minikv::index::LevelDbIndex index{path};
    const auto rec = index.getRecord("hello");

    EXPECT_TRUE(rec.rvolumes.empty());
    EXPECT_EQ(rec.deleted, minikv::record::Deleted::HARD);
    EXPECT_TRUE(rec.hash.empty());
  }

  cleanup();
}

TEST(IndexTest, StoresAndLoadsRecord) {
  const auto path = testDbPath("stores-and-loads");
  const auto cleanup = [&] { std::filesystem::remove_all(path); };

  {
    minikv::index::LevelDbIndex index{path};
    const auto rec = minikv::record::Record{
        {"volume-a", "volume-b"},
        minikv::record::Deleted::NO,
        "5d41402abc4b2a76b9719d911017c592",
    };

    EXPECT_TRUE(index.putRecord("hello", rec));

    const auto loaded = index.getRecord("hello");
    EXPECT_EQ(loaded.rvolumes, rec.rvolumes);
    EXPECT_EQ(loaded.deleted, rec.deleted);
    EXPECT_EQ(loaded.hash, rec.hash);
  }

  cleanup();
}

TEST(IndexTest, DeleteRecordRemovesKey) {
  const auto path = testDbPath("delete-record");
  const auto cleanup = [&] { std::filesystem::remove_all(path); };

  {
    minikv::index::LevelDbIndex index{path};
    const auto rec = minikv::record::Record{
        {"volume-a"},
        minikv::record::Deleted::NO,
        "",
    };

    EXPECT_TRUE(index.putRecord("hello", rec));
    EXPECT_EQ(index.getRecord("hello").deleted, minikv::record::Deleted::NO);

    EXPECT_TRUE(index.deleteRecord("hello"));

    const auto deleted = index.getRecord("hello");
    EXPECT_TRUE(deleted.rvolumes.empty());
    EXPECT_EQ(deleted.deleted, minikv::record::Deleted::HARD);
    EXPECT_TRUE(deleted.hash.empty());
  }

  cleanup();
}
