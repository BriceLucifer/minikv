#include "cli.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace {

minikv::cli::ParseResult parse(std::initializer_list<std::string> args) {
  return minikv::cli::parseCommandLine(std::vector<std::string>{args});
}

} // namespace

TEST(CliTest, ParsesServerCommandAndGoStyleDefaults) {
  const auto result = parse({
      "-db",
      "/tmp/indexdb",
      "-volumes",
      "localhost:3001,localhost:3002,localhost:3003",
      "server",
  });

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.options.command, "server");
  EXPECT_EQ(result.options.port, 3000);
  EXPECT_EQ(result.options.app.db_path, "/tmp/indexdb");
  EXPECT_EQ(result.options.app.volumes,
            (std::vector<std::string>{
                "localhost:3001",
                "localhost:3002",
                "localhost:3003",
            }));
  EXPECT_EQ(result.options.app.replicas, 3);
  EXPECT_EQ(result.options.app.subvolumes, 10);
  EXPECT_FALSE(result.options.app.protect);
  EXPECT_TRUE(result.options.app.md5sum);
  EXPECT_EQ(result.options.app.volume_timeout, std::chrono::seconds{1});
}

TEST(CliTest, ParsesConfiguredFlags) {
  const auto result = parse({
      "-port",
      "4000",
      "-db",
      "/tmp/db",
      "-fallback",
      "fallback:3009",
      "-replicas",
      "2",
      "-subvolumes",
      "4",
      "-volumes",
      "a:1,b:2",
      "-protect",
      "-md5sum=false",
      "-voltimeout",
      "250ms",
      "-v",
      "server",
  });

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.options.port, 4000);
  EXPECT_EQ(result.options.app.fallback, "fallback:3009");
  EXPECT_EQ(result.options.app.replicas, 2);
  EXPECT_EQ(result.options.app.subvolumes, 4);
  EXPECT_EQ(result.options.app.volumes,
            (std::vector<std::string>{"a:1", "b:2"}));
  EXPECT_TRUE(result.options.app.protect);
  EXPECT_FALSE(result.options.app.md5sum);
  EXPECT_EQ(result.options.app.volume_timeout, std::chrono::milliseconds{250});
  EXPECT_TRUE(result.options.verbose);
}

TEST(CliTest, RejectsMissingCommand) {
  const auto result = parse({
      "-db",
      "/tmp/db",
      "-volumes",
      "a:1,b:2,c:3",
  });

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("Usage: ./mkv"), std::string::npos);
}

TEST(CliTest, RejectsMissingDatabasePath) {
  const auto result = parse({
      "-volumes",
      "a:1,b:2,c:3",
      "server",
  });

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "Need a path to the database");
}

TEST(CliTest, RejectsTooFewVolumesForReplicas) {
  const auto result = parse({
      "-db",
      "/tmp/db",
      "-volumes",
      "a:1,b:2",
      "-replicas",
      "3",
      "server",
  });

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "Need at least as many volumes as replicas");
}

TEST(CliTest, AcceptsRebuildAndRebalanceCommandsForParity) {
  const auto rebuild = parse({
      "-db",
      "/tmp/db",
      "-volumes",
      "a:1,b:2,c:3",
      "rebuild",
  });
  const auto rebalance = parse({
      "-db",
      "/tmp/db",
      "-volumes",
      "a:1,b:2,c:3",
      "rebalance",
  });

  EXPECT_TRUE(rebuild.ok) << rebuild.error;
  EXPECT_EQ(rebuild.options.command, "rebuild");
  EXPECT_TRUE(rebalance.ok) << rebalance.error;
  EXPECT_EQ(rebalance.options.command, "rebalance");
}
