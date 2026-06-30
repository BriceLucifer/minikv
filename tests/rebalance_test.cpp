#include "rebalance.hpp"

#include "index.hpp"
#include "placement.hpp"
#include "record.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::filesystem::path testDbPath(std::string_view name) {
  auto path = std::filesystem::temp_directory_path();
  path /= "minikv-rebalance-test";
  path /= name;
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path;
}

class LocalVolumeServer {
public:
  httplib::Server server;

  void start() {
    port_ = server.bind_to_any_port("127.0.0.1");
    if (port_ < 0) {
      throw std::runtime_error("failed to bind test volume server");
    }

    worker_ = std::thread([this] { server.listen_after_bind(); });
    server.wait_until_ready();
  }

  std::string volume() const {
    return "127.0.0.1:" + std::to_string(port_);
  }

  ~LocalVolumeServer() {
    server.stop();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

private:
  int port_ = -1;
  std::thread worker_;
};

} // namespace

TEST(RebalanceTest, ComputesMissingAndStaleVolumes) {
  EXPECT_EQ(minikv::rebalance::missingTargetVolumes({"a", "c"}, {"a", "b"}),
            (std::vector<std::string>{"b"}));
  EXPECT_EQ(minikv::rebalance::staleRealVolumes({"a", "c"}, {"a", "b"}),
            (std::vector<std::string>{"c"}));
}

TEST(RebalanceTest, RebalanceObjectCopiesToTargetAndDeletesStaleReplica) {
  const auto path = testDbPath("copy-delete");
  const auto cleanup = [&] { std::filesystem::remove_all(path); };

  std::string source_path;
  auto source_deleted = false;
  LocalVolumeServer source;
  source.server.Get(R"(/.*)", [&](const httplib::Request &req,
                                  httplib::Response &res) {
    source_path = req.path;
    res.status = 200;
    res.set_content("payload", "application/octet-stream");
  });
  source.server.Delete(R"(/.*)", [&](const httplib::Request &,
                                     httplib::Response &res) {
    source_deleted = true;
    res.status = 204;
  });
  source.start();

  std::string target_path;
  std::string target_body;
  LocalVolumeServer target;
  target.server.Get(R"(/.*)", [](const httplib::Request &,
                                 httplib::Response &res) { res.status = 404; });
  target.server.Put(R"(/.*)", [&](const httplib::Request &req,
                                  httplib::Response &res) {
    target_path = req.path;
    target_body = req.body;
    res.status = 201;
  });
  target.start();

  {
    auto index = minikv::index::LevelDbIndex{path};
    const auto key = std::string{"/hello"};
    ASSERT_TRUE(index.putRecord(
        key, minikv::record::Record{
                 .rvolumes = {source.volume()},
                 .deleted = minikv::record::Deleted::NO,
                 .hash = "321c3cf486ed509164edec1e1981fec8",
             }));

    const auto options = minikv::rebalance::Options{
        .db_path = path,
        .volumes = {target.volume()},
        .replicas = 1,
        .subvolumes = 1,
    };

    ASSERT_TRUE(minikv::rebalance::rebalanceObject(index, options, key,
                                                   {source.volume()}));

    const auto expected_path = minikv::placement::key2path(key);
    EXPECT_EQ(source_path, expected_path);
    EXPECT_EQ(target_path, expected_path);
    EXPECT_EQ(target_body, "payload");
    EXPECT_TRUE(source_deleted);

    const auto rec = index.getRecord(key);
    EXPECT_EQ(rec.deleted, minikv::record::Deleted::NO);
    EXPECT_EQ(rec.rvolumes, (std::vector<std::string>{target.volume()}));
    EXPECT_TRUE(rec.hash.empty());
  }

  cleanup();
}

TEST(RebalanceTest, RebalanceObjectFailsWhenNoRecordedReplicaExists) {
  const auto path = testDbPath("missing");
  const auto cleanup = [&] { std::filesystem::remove_all(path); };

  LocalVolumeServer missing;
  missing.server.Get(R"(/.*)", [](const httplib::Request &,
                                  httplib::Response &res) {
    res.status = 404;
  });
  missing.start();

  {
    auto index = minikv::index::LevelDbIndex{path};
    const auto options = minikv::rebalance::Options{
        .db_path = path,
        .volumes = {missing.volume()},
        .replicas = 1,
        .subvolumes = 1,
    };

    EXPECT_FALSE(minikv::rebalance::rebalanceObject(
        index, options, "/missing", {missing.volume()}));
  }

  cleanup();
}
