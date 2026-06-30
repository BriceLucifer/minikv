#include "server.hpp"
#include "record.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::filesystem::path testDbPath(std::string_view name) {
  auto path = std::filesystem::temp_directory_path();
  path /= "minikv-server-test";
  path /= name;
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path;
}

minikv::server::AppOptions appOptions(std::string_view name) {
  return minikv::server::AppOptions{
      .db_path = testDbPath(name),
      .volumes = {"volume-a", "volume-b", "volume-c"},
      .fallback = "fallback-volume",
      .replicas = 2,
      .subvolumes = 4,
      .protect = true,
      .md5sum = false,
      .volume_timeout = std::chrono::milliseconds{250},
  };
}

} // namespace

TEST(ServerAppTest, StoresOptions) {
  const auto options = appOptions("stores-options");
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    const minikv::server::App app{options};

    EXPECT_EQ(app.options().db_path, options.db_path);
    EXPECT_EQ(app.options().volumes, options.volumes);
    EXPECT_EQ(app.options().fallback, options.fallback);
    EXPECT_EQ(app.options().replicas, options.replicas);
    EXPECT_EQ(app.options().subvolumes, options.subvolumes);
    EXPECT_EQ(app.options().protect, options.protect);
    EXPECT_EQ(app.options().md5sum, options.md5sum);
    EXPECT_EQ(app.options().volume_timeout, options.volume_timeout);
  }

  cleanup();
}

TEST(ServerAppTest, LockKeyRejectsDuplicateUntilUnlocked) {
  const auto options = appOptions("lock-key");
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};

    EXPECT_TRUE(app.lockKey("hello"));
    EXPECT_FALSE(app.lockKey("hello"));
    EXPECT_TRUE(app.lockKey("world"));

    app.unlockKey("hello");

    EXPECT_TRUE(app.lockKey("hello"));
  }

  cleanup();
}

TEST(ServerAppTest, ProxiesRecordOperationsToIndex) {
  const auto options = appOptions("record-operations");
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    const auto rec = minikv::record::Record{
        {"volume-a", "volume-b"},
        minikv::record::Deleted::NO,
        "5d41402abc4b2a76b9719d911017c592",
    };

    EXPECT_EQ(app.getRecord("hello").deleted, minikv::record::Deleted::HARD);

    EXPECT_TRUE(app.putRecord("hello", rec));

    const auto loaded = app.getRecord("hello");
    EXPECT_EQ(loaded.rvolumes, rec.rvolumes);
    EXPECT_EQ(loaded.deleted, rec.deleted);
    EXPECT_EQ(loaded.hash, rec.hash);

    EXPECT_TRUE(app.deleteRecord("hello"));
    EXPECT_EQ(app.getRecord("hello").deleted, minikv::record::Deleted::HARD);
  }

  cleanup();
}
