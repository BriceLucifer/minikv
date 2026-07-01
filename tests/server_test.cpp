#include "server.hpp"

#include "hash.hpp"
#include "placement.hpp"
#include "record.hpp"

#include <gtest/gtest.h>
#include "http_test_util.hpp"
#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
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

std::vector<std::string> jsonStringArray(const boost::json::value &value) {
  auto out = std::vector<std::string>{};
  for (const auto &item : value.as_array()) {
    const auto string = item.as_string();
    out.emplace_back(string.data(), string.size());
  }
  return out;
}

std::string xmlValue(std::string_view body, std::string_view tag) {
  const auto open_tag = "<" + std::string{tag} + ">";
  const auto close_tag = "</" + std::string{tag} + ">";
  const auto open = body.find(open_tag);
  if (open == std::string_view::npos) {
    return "";
  }
  const auto value_start = open + open_tag.size();
  const auto close = body.find(close_tag, value_start);
  if (close == std::string_view::npos) {
    return "";
  }
  return std::string{body.substr(value_start, close - value_start)};
}

class LocalVolumeServer {
public:
  minikv::test::TestHttpServer server;

  void start() {
    port_ = server.bindToAnyPort("127.0.0.1");
    if (port_ < 0) {
      throw std::runtime_error("failed to bind test volume server");
    }

    worker_ = std::thread([this] {
      server.listenAfterBind();
    });
    server.waitUntilReady();
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

TEST(ServerProbeOrderTest, ReturnsCompletePermutation) {
  auto rng = std::mt19937{7};
  auto order = minikv::server::replicaProbeOrder(5, rng);

  ASSERT_EQ(order.size(), 5U);
  std::ranges::sort(order);
  EXPECT_EQ(order, (std::vector<std::size_t>{0, 1, 2, 3, 4}));
}

TEST(ServerProbeOrderTest, ShufflesOrderForReadDistribution) {
  auto saw_non_identity = false;
  for (auto seed = 1U; seed <= 20U; ++seed) {
    auto rng = std::mt19937{seed};
    const auto order = minikv::server::replicaProbeOrder(4, rng);
    if (order != (std::vector<std::size_t>{0, 1, 2, 3})) {
      saw_non_identity = true;
      break;
    }
  }

  EXPECT_TRUE(saw_non_identity);
}

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

TEST(ServerAppTest, WriteToReplicasStoresRemoteBodyAndFinalRecord) {
  std::string received_path;
  std::string received_body;

  LocalVolumeServer volume;
  volume.server.Put(R"(/.*)", [&](const minikv::http::Request &req,
                                  minikv::http::Response &res) {
    received_path = req.path;
    received_body = req.body;
    res.status = 201;
  });
  volume.start();

  auto options = appOptions("write-to-replicas");
  options.volumes = {volume.volume()};
  options.replicas = 1;
  options.subvolumes = 1;
  options.md5sum = true;
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};

    const auto result = app.writeToReplicas("/hello", "payload");

    EXPECT_EQ(result.status, 201);
    EXPECT_EQ(result.record.rvolumes, options.volumes);
    EXPECT_EQ(result.record.deleted, minikv::record::Deleted::NO);
    EXPECT_EQ(result.record.hash, minikv::md5_hex("payload"));
    EXPECT_EQ(received_path, minikv::placement::key2path("/hello"));
    EXPECT_EQ(received_body, "payload");

    const auto loaded = app.getRecord("/hello");
    EXPECT_EQ(loaded.rvolumes, result.record.rvolumes);
    EXPECT_EQ(loaded.deleted, result.record.deleted);
    EXPECT_EQ(loaded.hash, result.record.hash);
  }

  cleanup();
}

TEST(ServerAppTest, ReadFromReplicaReturnsRedirectForLiveRecord) {
  LocalVolumeServer volume;
  volume.server.Get(R"(/.*)", [](const minikv::http::Request &,
                                 minikv::http::Response &res) {
    res.status = 200;
  });
  volume.start();

  auto options = appOptions("read-redirect");
  options.volumes = {volume.volume()};
  options.replicas = 1;
  options.subvolumes = 1;
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    const auto rec = minikv::record::Record{
        .rvolumes = {volume.volume()},
        .deleted = minikv::record::Deleted::NO,
        .hash = "321c3cf486ed509164edec1e1981fec8",
    };
    ASSERT_TRUE(app.putRecord("/hello", rec));

    const auto result = app.readFromReplica("/hello");

    EXPECT_EQ(result.status, 302);
    EXPECT_EQ(result.redirect_url,
              "http://" + volume.volume() + minikv::placement::key2path("/hello"));
    EXPECT_EQ(result.record.rvolumes, rec.rvolumes);
    EXPECT_EQ(result.record.deleted, rec.deleted);
    EXPECT_EQ(result.record.hash, rec.hash);
    EXPECT_EQ(result.content_md5, rec.hash);
    EXPECT_EQ(result.key_volumes, volume.volume());
    EXPECT_EQ(result.key_balance, "balanced");
  }

  cleanup();
}

TEST(ServerAppTest, ReadFromReplicaReturnsNotFoundForMissingKey) {
  auto options = appOptions("read-missing");
  options.fallback = "";
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};

    const auto result = app.readFromReplica("/missing");

    EXPECT_EQ(result.status, 404);
    EXPECT_TRUE(result.redirect_url.empty());
    EXPECT_EQ(result.record.deleted, minikv::record::Deleted::HARD);
    EXPECT_TRUE(result.record.rvolumes.empty());
  }

  cleanup();
}

TEST(ServerAppTest, ReadFromReplicaRedirectsMissingKeyToFallback) {
  auto options = appOptions("read-fallback");
  options.fallback = "fallback-volume";
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};

    const auto result = app.readFromReplica("/missing");

    EXPECT_EQ(result.status, 302);
    EXPECT_EQ(result.redirect_url, "http://fallback-volume/missing");
    EXPECT_EQ(result.record.deleted, minikv::record::Deleted::HARD);
  }

  cleanup();
}

TEST(ServerAppTest, ReadFromReplicaReturnsNotFoundForSoftDeletedRecord) {
  auto options = appOptions("read-soft-deleted");
  options.fallback = "";
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    const auto rec = minikv::record::Record{
        .rvolumes = {"volume-a"},
        .deleted = minikv::record::Deleted::SOFT,
        .hash = "",
    };
    ASSERT_TRUE(app.putRecord("/hello", rec));

    const auto result = app.readFromReplica("/hello");

    EXPECT_EQ(result.status, 404);
    EXPECT_TRUE(result.redirect_url.empty());
    EXPECT_EQ(result.record.rvolumes, rec.rvolumes);
    EXPECT_EQ(result.record.deleted, rec.deleted);
  }

  cleanup();
}

TEST(ServerAppTest, ReadFromReplicaReturnsNotFoundForRecordWithoutVolumes) {
  auto options = appOptions("read-empty-volumes");
  options.fallback = "";
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    const auto rec = minikv::record::Record{
        .rvolumes = {},
        .deleted = minikv::record::Deleted::NO,
        .hash = "",
    };
    ASSERT_TRUE(app.putRecord("/hello", rec));

    const auto result = app.readFromReplica("/hello");

    EXPECT_EQ(result.status, 404);
    EXPECT_TRUE(result.redirect_url.empty());
    EXPECT_TRUE(result.record.rvolumes.empty() ||
                result.record.rvolumes.front().empty());
    EXPECT_EQ(result.record.deleted, rec.deleted);
  }

  cleanup();
}

TEST(ServerAppTest, DeleteFromReplicasReturnsNotFoundForMissingKey) {
  const auto options = appOptions("delete-missing");
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};

    const auto result = app.deleteFromReplicas("/missing");

    EXPECT_EQ(result.status, 404);
    EXPECT_EQ(result.record.deleted, minikv::record::Deleted::HARD);
    EXPECT_TRUE(result.record.rvolumes.empty());
  }

  cleanup();
}

TEST(ServerAppTest, DeleteFromReplicasRespectsProtectForLiveRecord) {
  const auto options = appOptions("delete-protected");
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    const auto rec = minikv::record::Record{
        .rvolumes = {"volume-a"},
        .deleted = minikv::record::Deleted::NO,
        .hash = "",
    };
    ASSERT_TRUE(app.putRecord("/hello", rec));

    const auto result = app.deleteFromReplicas("/hello");

    EXPECT_EQ(result.status, 403);
    EXPECT_EQ(result.record.rvolumes, rec.rvolumes);
    EXPECT_EQ(result.record.deleted, rec.deleted);
    EXPECT_EQ(app.getRecord("/hello").deleted, minikv::record::Deleted::NO);
  }

  cleanup();
}

TEST(ServerAppTest, DeleteFromReplicasDeletesRemoteObjectsAndHardDeletesRecord) {
  std::vector<std::string> deleted_paths;

  LocalVolumeServer volume;
  volume.server.Delete(R"(/.*)", [&](const minikv::http::Request &req,
                                     minikv::http::Response &res) {
    deleted_paths.push_back(req.path);
    res.status = 204;
  });
  volume.start();

  auto options = appOptions("delete-remote");
  options.volumes = {volume.volume()};
  options.replicas = 1;
  options.subvolumes = 1;
  options.protect = false;
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    const auto rec = minikv::record::Record{
        .rvolumes = {volume.volume()},
        .deleted = minikv::record::Deleted::NO,
        .hash = "321c3cf486ed509164edec1e1981fec8",
    };
    ASSERT_TRUE(app.putRecord("/hello", rec));

    const auto result = app.deleteFromReplicas("/hello");

    ASSERT_EQ(deleted_paths.size(), 1U);
    EXPECT_EQ(deleted_paths.front(), minikv::placement::key2path("/hello"));
    EXPECT_EQ(result.status, 204);
    EXPECT_EQ(result.record.deleted, minikv::record::Deleted::HARD);
    EXPECT_EQ(app.getRecord("/hello").deleted, minikv::record::Deleted::HARD);
  }

  cleanup();
}

TEST(ServerAppTest, DeleteFromReplicasKeepsSoftRecordWhenRemoteDeleteFails) {
  LocalVolumeServer volume;
  volume.server.Delete(R"(/.*)", [](const minikv::http::Request &,
                                    minikv::http::Response &res) {
    res.status = 500;
  });
  volume.start();

  auto options = appOptions("delete-remote-fails");
  options.volumes = {volume.volume()};
  options.replicas = 1;
  options.subvolumes = 1;
  options.protect = false;
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    const auto rec = minikv::record::Record{
        .rvolumes = {volume.volume()},
        .deleted = minikv::record::Deleted::NO,
        .hash = "321c3cf486ed509164edec1e1981fec8",
    };
    ASSERT_TRUE(app.putRecord("/hello", rec));

    const auto result = app.deleteFromReplicas("/hello");

    EXPECT_EQ(result.status, 500);
    EXPECT_EQ(result.record.deleted, minikv::record::Deleted::SOFT);
    EXPECT_EQ(result.record.rvolumes, rec.rvolumes);
    EXPECT_EQ(result.record.hash, rec.hash);

    const auto loaded = app.getRecord("/hello");
    EXPECT_EQ(loaded.deleted, minikv::record::Deleted::SOFT);
    EXPECT_EQ(loaded.rvolumes, rec.rvolumes);
    EXPECT_EQ(loaded.hash, rec.hash);
  }

  cleanup();
}

TEST(ServerAppTest, DeleteFromReplicasUnlinkOnlySoftDeletesMetadata) {
  LocalVolumeServer volume;
  auto remote_delete_called = false;
  volume.server.Delete(R"(/.*)", [&](const minikv::http::Request &,
                                     minikv::http::Response &res) {
    remote_delete_called = true;
    res.status = 204;
  });
  volume.start();

  auto options = appOptions("delete-unlink");
  options.volumes = {volume.volume()};
  options.replicas = 1;
  options.subvolumes = 1;
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    const auto rec = minikv::record::Record{
        .rvolumes = {volume.volume()},
        .deleted = minikv::record::Deleted::NO,
        .hash = "321c3cf486ed509164edec1e1981fec8",
    };
    ASSERT_TRUE(app.putRecord("/hello", rec));

    const auto result = app.deleteFromReplicas("/hello", true);

    EXPECT_EQ(result.status, 204);
    EXPECT_EQ(result.record.deleted, minikv::record::Deleted::SOFT);
    EXPECT_FALSE(remote_delete_called);

    const auto loaded = app.getRecord("/hello");
    EXPECT_EQ(loaded.deleted, minikv::record::Deleted::SOFT);
    EXPECT_EQ(loaded.rvolumes, rec.rvolumes);
    EXPECT_EQ(loaded.hash, rec.hash);
  }

  cleanup();
}

TEST(ServerAppTest, DeleteFromReplicasUnlinkReturnsNotFoundForSoftDeletedRecord) {
  const auto options = appOptions("delete-unlink-soft");
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    const auto rec = minikv::record::Record{
        .rvolumes = {"volume-a"},
        .deleted = minikv::record::Deleted::SOFT,
        .hash = "",
    };
    ASSERT_TRUE(app.putRecord("/hello", rec));

    const auto result = app.deleteFromReplicas("/hello", true);

    EXPECT_EQ(result.status, 404);
    EXPECT_EQ(result.record.deleted, minikv::record::Deleted::SOFT);
  }

  cleanup();
}

TEST(ServerRouteTest, PutRouteWritesToReplicas) {
  std::string received_body;

  LocalVolumeServer volume;
  volume.server.Put(R"(/.*)", [&](const minikv::http::Request &req,
                                  minikv::http::Response &res) {
    received_body = req.body;
    res.status = 201;
  });
  volume.start();

  auto options = appOptions("route-put");
  options.volumes = {volume.volume()};
  options.replicas = 1;
  options.subvolumes = 1;
  options.md5sum = false;
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());
    const auto res = client.Put("/hello", "payload", "application/octet-stream");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201);
    EXPECT_EQ(received_body, "payload");

    const auto rec = app.getRecord("/hello");
    EXPECT_EQ(rec.deleted, minikv::record::Deleted::NO);
    EXPECT_EQ(rec.rvolumes, options.volumes);
  }

  cleanup();
}

TEST(ServerRouteTest, PutRouteRejectsEmptyBodyWithoutWritingRecord) {
  auto remote_put_called = false;

  LocalVolumeServer volume;
  volume.server.Put(R"(/.*)", [&](const minikv::http::Request &,
                                  minikv::http::Response &res) {
    remote_put_called = true;
    res.status = 201;
  });
  volume.start();

  auto options = appOptions("route-put-empty");
  options.volumes = {volume.volume()};
  options.replicas = 1;
  options.subvolumes = 1;
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());
    const auto res = client.Put("/hello", "", "application/octet-stream");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 411);
    EXPECT_FALSE(remote_put_called);
    EXPECT_EQ(app.getRecord("/hello").deleted, minikv::record::Deleted::HARD);

    const auto retry = client.Put("/hello", "payload", "application/octet-stream");
    ASSERT_TRUE(retry);
    EXPECT_EQ(retry->status, 201);
    EXPECT_EQ(app.getRecord("/hello").deleted, minikv::record::Deleted::NO);
  }

  cleanup();
}

TEST(ServerRouteTest, PutRouteRejectsOverwriteOfLiveRecord) {
  auto remote_put_called = false;

  LocalVolumeServer volume;
  volume.server.Put(R"(/.*)", [&](const minikv::http::Request &,
                                  minikv::http::Response &res) {
    remote_put_called = true;
    res.status = 201;
  });
  volume.start();

  auto options = appOptions("route-put-overwrite");
  options.volumes = {volume.volume()};
  options.replicas = 1;
  options.subvolumes = 1;
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    const auto rec = minikv::record::Record{
        .rvolumes = {volume.volume()},
        .deleted = minikv::record::Deleted::NO,
        .hash = "321c3cf486ed509164edec1e1981fec8",
    };
    ASSERT_TRUE(app.putRecord("/hello", rec));

    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());
    const auto res = client.Put("/hello", "payload", "application/octet-stream");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
    EXPECT_FALSE(remote_put_called);

    const auto loaded = app.getRecord("/hello");
    EXPECT_EQ(loaded.rvolumes, rec.rvolumes);
    EXPECT_EQ(loaded.deleted, rec.deleted);
    EXPECT_EQ(loaded.hash, rec.hash);
  }

  cleanup();
}

TEST(ServerRouteTest, MutatingRoutesReturnConflictWhenKeyIsLocked) {
  auto remote_put_called = false;
  auto remote_delete_called = false;

  LocalVolumeServer volume;
  volume.server.Put(R"(/.*)", [&](const minikv::http::Request &,
                                  minikv::http::Response &res) {
    remote_put_called = true;
    res.status = 201;
  });
  volume.server.Delete(R"(/.*)", [&](const minikv::http::Request &,
                                     minikv::http::Response &res) {
    remote_delete_called = true;
    res.status = 204;
  });
  volume.start();

  auto options = appOptions("route-mutating-conflict");
  options.volumes = {volume.volume()};
  options.replicas = 1;
  options.subvolumes = 1;
  options.protect = false;
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    ASSERT_TRUE(app.putRecord(
        "/delete-me",
        minikv::record::Record{
            .rvolumes = {volume.volume()},
            .deleted = minikv::record::Deleted::NO,
            .hash = "",
        }));

    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());

    ASSERT_TRUE(app.lockKey("/hello"));
    const auto put_conflict =
        client.Put("/hello", "payload", "application/octet-stream");
    app.unlockKey("/hello");

    ASSERT_TRUE(put_conflict);
    EXPECT_EQ(put_conflict->status, 409);
    EXPECT_FALSE(remote_put_called);
    EXPECT_EQ(app.getRecord("/hello").deleted, minikv::record::Deleted::HARD);

    ASSERT_TRUE(app.lockKey("/delete-me"));
    const auto delete_conflict = client.Delete("/delete-me");
    app.unlockKey("/delete-me");

    ASSERT_TRUE(delete_conflict);
    EXPECT_EQ(delete_conflict->status, 409);
    EXPECT_FALSE(remote_delete_called);
    EXPECT_EQ(app.getRecord("/delete-me").deleted, minikv::record::Deleted::NO);
  }

  cleanup();
}

TEST(ServerRouteTest, GetAndHeadRoutesReturnRedirectLocation) {
  LocalVolumeServer volume;
  volume.server.Get(R"(/.*)", [](const minikv::http::Request &,
                                 minikv::http::Response &res) {
    res.status = 200;
  });
  volume.start();

  auto options = appOptions("route-read");
  options.volumes = {volume.volume()};
  options.replicas = 1;
  options.subvolumes = 1;
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    ASSERT_TRUE(app.putRecord(
        "/hello",
        minikv::record::Record{
            .rvolumes = {volume.volume()},
            .deleted = minikv::record::Deleted::NO,
            .hash = "321c3cf486ed509164edec1e1981fec8",
        }));

    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());
    const auto expected_location =
        "http://" + volume.volume() + minikv::placement::key2path("/hello");

    const auto get_res = client.Get("/hello");
    ASSERT_TRUE(get_res);
    EXPECT_EQ(get_res->status, 302);
    EXPECT_EQ(get_res->get_header_value("Location"), expected_location);
    EXPECT_EQ(get_res->get_header_value("Content-Md5"),
              "321c3cf486ed509164edec1e1981fec8");
    EXPECT_EQ(get_res->get_header_value("Key-Volumes"), volume.volume());
    EXPECT_EQ(get_res->get_header_value("Key-Balance"), "balanced");

    const auto head_res = client.Head("/hello");
    ASSERT_TRUE(head_res);
    EXPECT_EQ(head_res->status, 302);
    EXPECT_EQ(head_res->get_header_value("Location"), expected_location);
    EXPECT_EQ(head_res->get_header_value("Content-Md5"),
              "321c3cf486ed509164edec1e1981fec8");
    EXPECT_EQ(head_res->get_header_value("Key-Volumes"), volume.volume());
    EXPECT_EQ(head_res->get_header_value("Key-Balance"), "balanced");
  }

  cleanup();
}

TEST(ServerRouteTest, QueryListReturnsLiveKeysAsJson) {
  const auto options = appOptions("route-query-list");
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    ASSERT_TRUE(app.putRecord(
        "/runs/1",
        minikv::record::Record{
            .rvolumes = {"volume-a"},
            .deleted = minikv::record::Deleted::NO,
            .hash = "",
        }));
    ASSERT_TRUE(app.putRecord(
        "/runs/2",
        minikv::record::Record{
            .rvolumes = {"volume-a"},
            .deleted = minikv::record::Deleted::NO,
            .hash = "",
        }));
    ASSERT_TRUE(app.putRecord(
        "/runs/deleted",
        minikv::record::Record{
            .rvolumes = {"volume-a"},
            .deleted = minikv::record::Deleted::SOFT,
            .hash = "",
        }));
    ASSERT_TRUE(app.putRecord(
        "/other/1",
        minikv::record::Record{
            .rvolumes = {"volume-a"},
            .deleted = minikv::record::Deleted::NO,
            .hash = "",
        }));

    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());
    const auto res = client.Get("/runs?list");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_NE(res->get_header_value("Content-Type").find("application/json"),
              std::string::npos);

    const auto body = boost::json::parse(res->body).as_object();
    EXPECT_EQ(body.at("next").as_string(), "");
    EXPECT_EQ(jsonStringArray(body.at("keys")),
              (std::vector<std::string>{"/runs/1", "/runs/2"}));
  }

  cleanup();
}

TEST(ServerRouteTest, QueryUnlinkedReturnsSoftDeletedKeysAsJson) {
  const auto options = appOptions("route-query-unlinked");
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    ASSERT_TRUE(app.putRecord(
        "/live",
        minikv::record::Record{
            .rvolumes = {"volume-a"},
            .deleted = minikv::record::Deleted::NO,
            .hash = "",
        }));
    ASSERT_TRUE(app.putRecord(
        "/soft",
        minikv::record::Record{
            .rvolumes = {"volume-a"},
            .deleted = minikv::record::Deleted::SOFT,
            .hash = "",
        }));

    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());
    const auto res = client.Get("/?unlinked");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    const auto body = boost::json::parse(res->body).as_object();
    EXPECT_EQ(body.at("next").as_string(), "");
    EXPECT_EQ(jsonStringArray(body.at("keys")),
              (std::vector<std::string>{"/soft"}));
  }

  cleanup();
}

TEST(ServerRouteTest, QueryListSupportsLimitAndStart) {
  const auto options = appOptions("route-query-limit-start");
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    for (const auto *key : {"/runs/1", "/runs/2", "/runs/3"}) {
      ASSERT_TRUE(app.putRecord(
          key,
          minikv::record::Record{
              .rvolumes = {"volume-a"},
              .deleted = minikv::record::Deleted::NO,
              .hash = "",
          }));
    }

    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());
    const auto first = client.Get("/runs?list&limit=2");

    ASSERT_TRUE(first);
    EXPECT_EQ(first->status, 200);
    const auto first_body = boost::json::parse(first->body).as_object();
    EXPECT_EQ(first_body.at("next").as_string(), "/runs/3");
    EXPECT_EQ(jsonStringArray(first_body.at("keys")),
              (std::vector<std::string>{"/runs/1", "/runs/2"}));

    const auto second = client.Get("/runs?list&start=/runs/3");
    ASSERT_TRUE(second);
    EXPECT_EQ(second->status, 200);
    const auto second_body = boost::json::parse(second->body).as_object();
    EXPECT_EQ(second_body.at("next").as_string(), "");
    EXPECT_EQ(jsonStringArray(second_body.at("keys")),
              (std::vector<std::string>{"/runs/3"}));
  }

  cleanup();
}

TEST(ServerRouteTest, QueryRejectsInvalidLimitAndUnknownOperation) {
  const auto options = appOptions("route-query-invalid");
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());
    const auto invalid_limit = client.Get("/?list&limit=bogus");
    const auto unknown = client.Get("/?unknown");

    ASSERT_TRUE(invalid_limit);
    EXPECT_EQ(invalid_limit->status, 400);

    ASSERT_TRUE(unknown);
    EXPECT_EQ(unknown->status, 403);
  }

  cleanup();
}

TEST(ServerRouteTest, S3ListTypeTwoReturnsXmlKeysUnderBucketPrefix) {
  const auto options = appOptions("route-s3-list-type-two");
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    ASSERT_TRUE(app.putRecord(
        "/bucket/alpha.txt",
        minikv::record::Record{
            .rvolumes = {"volume-a"},
            .deleted = minikv::record::Deleted::NO,
            .hash = "",
        }));
    ASSERT_TRUE(app.putRecord(
        "/bucket/prefix/bravo&charlie.txt",
        minikv::record::Record{
            .rvolumes = {"volume-a"},
            .deleted = minikv::record::Deleted::NO,
            .hash = "",
        }));
    ASSERT_TRUE(app.putRecord(
        "/bucket/prefix/deleted.txt",
        minikv::record::Record{
            .rvolumes = {"volume-a"},
            .deleted = minikv::record::Deleted::SOFT,
            .hash = "",
        }));
    ASSERT_TRUE(app.putRecord(
        "/other/prefix/ignored.txt",
        minikv::record::Record{
            .rvolumes = {"volume-a"},
            .deleted = minikv::record::Deleted::NO,
            .hash = "",
        }));

    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());
    const auto res = client.Get("/bucket?list-type=2&prefix=prefix/");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_NE(res->get_header_value("Content-Type").find("application/xml"),
              std::string::npos);
    EXPECT_NE(res->body.find("<ListBucketResult>"), std::string::npos);
    EXPECT_NE(res->body.find("<Contents><Key>bravo&amp;charlie.txt</Key></Contents>"),
              std::string::npos);
    EXPECT_EQ(res->body.find("alpha.txt"), std::string::npos);
    EXPECT_EQ(res->body.find("deleted.txt"), std::string::npos);
    EXPECT_EQ(res->body.find("ignored.txt"), std::string::npos);
  }

  cleanup();
}

TEST(ServerRouteTest, DeleteRouteDeletesRemoteObjects) {
  auto remote_delete_called = false;

  LocalVolumeServer volume;
  volume.server.Delete(R"(/.*)", [&](const minikv::http::Request &,
                                     minikv::http::Response &res) {
    remote_delete_called = true;
    res.status = 204;
  });
  volume.start();

  auto options = appOptions("route-delete");
  options.volumes = {volume.volume()};
  options.replicas = 1;
  options.subvolumes = 1;
  options.protect = false;
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    ASSERT_TRUE(app.putRecord(
        "/hello",
        minikv::record::Record{
            .rvolumes = {volume.volume()},
            .deleted = minikv::record::Deleted::NO,
            .hash = "",
        }));

    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());
    const auto res = client.Delete("/hello");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 204);
    EXPECT_TRUE(remote_delete_called);
    EXPECT_EQ(app.getRecord("/hello").deleted, minikv::record::Deleted::HARD);
  }

  cleanup();
}

TEST(ServerRouteTest, S3BulkDeleteDeletesBucketChildKeys) {
  auto deleted_paths = std::vector<std::string>{};

  LocalVolumeServer volume;
  volume.server.Delete(R"(/.*)", [&](const minikv::http::Request &req,
                                     minikv::http::Response &res) {
    deleted_paths.push_back(req.path);
    res.status = 204;
  });
  volume.start();

  auto options = appOptions("route-s3-delete");
  options.volumes = {volume.volume()};
  options.replicas = 1;
  options.subvolumes = 1;
  options.protect = false;
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    ASSERT_TRUE(app.putRecord(
        "/bucket/alpha.txt",
        minikv::record::Record{
            .rvolumes = {volume.volume()},
            .deleted = minikv::record::Deleted::NO,
            .hash = "",
        }));
    ASSERT_TRUE(app.putRecord(
        "/bucket/bravo&charlie.txt",
        minikv::record::Record{
            .rvolumes = {volume.volume()},
            .deleted = minikv::record::Deleted::NO,
            .hash = "",
        }));

    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());
    const auto xml =
        std::string{"<Delete><Object><Key>alpha.txt</Key></Object>"
                    "<Object><Key>bravo&amp;charlie.txt</Key></Object></Delete>"};
    const auto res = client.Post("/bucket?delete", xml);

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 204);
    ASSERT_EQ(deleted_paths.size(), 2U);
    EXPECT_EQ(deleted_paths[0], minikv::placement::key2path("/bucket/alpha.txt"));
    EXPECT_EQ(deleted_paths[1],
              minikv::placement::key2path("/bucket/bravo&charlie.txt"));
    EXPECT_EQ(app.getRecord("/bucket/alpha.txt").deleted,
              minikv::record::Deleted::HARD);
    EXPECT_EQ(app.getRecord("/bucket/bravo&charlie.txt").deleted,
              minikv::record::Deleted::HARD);
  }

  cleanup();
}

TEST(ServerRouteTest, S3BulkDeleteRejectsMalformedXmlWithoutDeleting) {
  auto remote_delete_called = false;

  LocalVolumeServer volume;
  volume.server.Delete(R"(/.*)", [&](const minikv::http::Request &,
                                     minikv::http::Response &res) {
    remote_delete_called = true;
    res.status = 204;
  });
  volume.start();

  auto options = appOptions("route-s3-delete-malformed");
  options.volumes = {volume.volume()};
  options.replicas = 1;
  options.subvolumes = 1;
  options.protect = false;
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    ASSERT_TRUE(app.putRecord(
        "/bucket/alpha.txt",
        minikv::record::Record{
            .rvolumes = {volume.volume()},
            .deleted = minikv::record::Deleted::NO,
            .hash = "",
        }));

    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());
    const auto res =
        client.Post("/bucket?delete", "<Delete><Object><Key>alpha.txt</Object></Delete>");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 500);
    EXPECT_FALSE(remote_delete_called);
    EXPECT_EQ(app.getRecord("/bucket/alpha.txt").deleted,
              minikv::record::Deleted::NO);
  }

  cleanup();
}

TEST(ServerRouteTest, S3MultipartUploadCombinesPartsAndWritesReplicas) {
  auto received_body = std::string{};

  LocalVolumeServer volume;
  volume.server.Put(R"(/.*)", [&](const minikv::http::Request &req,
                                  minikv::http::Response &res) {
    received_body = req.body;
    res.status = 201;
  });
  volume.start();

  auto options = appOptions("route-s3-multipart");
  options.volumes = {volume.volume()};
  options.replicas = 1;
  options.subvolumes = 1;
  options.md5sum = true;
  const auto cleanup = [&] {
    std::filesystem::remove_all(options.db_path);
    auto multipart_path = options.db_path;
    multipart_path += ".multipart";
    std::filesystem::remove_all(multipart_path);
  };

  {
    minikv::server::App app{options};
    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());
    const auto init = client.Post("/bucket/object?uploads", "");

    ASSERT_TRUE(init);
    EXPECT_EQ(init->status, 200);
    const auto upload_id = xmlValue(init->body, "UploadId");
    ASSERT_FALSE(upload_id.empty());

    const auto second =
        client.Put("/bucket/object?partNumber=2&uploadId=" + upload_id,
                   "world", "application/octet-stream");
    ASSERT_TRUE(second);
    EXPECT_EQ(second->status, 200);

    const auto first =
        client.Put("/bucket/object?partNumber=1&uploadId=" + upload_id,
                   "hello ", "application/octet-stream");
    ASSERT_TRUE(first);
    EXPECT_EQ(first->status, 200);

    const auto complete_xml =
        std::string{"<CompleteMultipartUpload>"
                    "<Part><PartNumber>1</PartNumber></Part>"
                    "<Part><PartNumber>2</PartNumber></Part>"
                    "</CompleteMultipartUpload>"};
    const auto complete =
        client.Post("/bucket/object?uploadId=" + upload_id, complete_xml);

    ASSERT_TRUE(complete);
    EXPECT_EQ(complete->status, 201);
    EXPECT_NE(complete->body.find("<CompleteMultipartUploadResult>"),
              std::string::npos);
    EXPECT_EQ(received_body, "hello world");

    const auto rec = app.getRecord("/bucket/object");
    EXPECT_EQ(rec.deleted, minikv::record::Deleted::NO);
    EXPECT_EQ(rec.rvolumes, (std::vector<std::string>{volume.volume()}));
    EXPECT_EQ(rec.hash, minikv::md5_hex("hello world"));
  }

  cleanup();
}

TEST(ServerRouteTest, S3MultipartRejectsUnknownUploadIdAndMissingParts) {
  auto remote_put_called = false;

  LocalVolumeServer volume;
  volume.server.Put(R"(/.*)", [&](const minikv::http::Request &,
                                  minikv::http::Response &res) {
    remote_put_called = true;
    res.status = 201;
  });
  volume.start();

  auto options = appOptions("route-s3-multipart-invalid");
  options.volumes = {volume.volume()};
  options.replicas = 1;
  options.subvolumes = 1;
  const auto cleanup = [&] {
    std::filesystem::remove_all(options.db_path);
    auto multipart_path = options.db_path;
    multipart_path += ".multipart";
    std::filesystem::remove_all(multipart_path);
  };

  {
    minikv::server::App app{options};
    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());
    const auto unknown_part =
        client.Put("/bucket/object?partNumber=1&uploadId=missing", "body",
                   "application/octet-stream");
    ASSERT_TRUE(unknown_part);
    EXPECT_EQ(unknown_part->status, 403);

    const auto init = client.Post("/bucket/object?uploads", "");
    ASSERT_TRUE(init);
    ASSERT_EQ(init->status, 200);
    const auto upload_id = xmlValue(init->body, "UploadId");
    ASSERT_FALSE(upload_id.empty());

    const auto complete_xml =
        std::string{"<CompleteMultipartUpload>"
                    "<Part><PartNumber>1</PartNumber></Part>"
                    "</CompleteMultipartUpload>"};
    const auto missing_part =
        client.Post("/bucket/object?uploadId=" + upload_id, complete_xml);
    ASSERT_TRUE(missing_part);
    EXPECT_EQ(missing_part->status, 403);
    EXPECT_FALSE(remote_put_called);
    EXPECT_EQ(app.getRecord("/bucket/object").deleted,
              minikv::record::Deleted::HARD);
  }

  cleanup();
}

TEST(ServerRouteTest, S3MultipartMissingPartCanBeUploadedAndRetried) {
  auto received_body = std::string{};

  LocalVolumeServer volume;
  volume.server.Put(R"(/.*)", [&](const minikv::http::Request &req,
                                  minikv::http::Response &res) {
    received_body = req.body;
    res.status = 201;
  });
  volume.start();

  auto options = appOptions("route-s3-multipart-retry-missing");
  options.volumes = {volume.volume()};
  options.replicas = 1;
  options.subvolumes = 1;
  const auto cleanup = [&] {
    std::filesystem::remove_all(options.db_path);
    auto multipart_path = options.db_path;
    multipart_path += ".multipart";
    std::filesystem::remove_all(multipart_path);
  };

  {
    minikv::server::App app{options};
    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());
    const auto init = client.Post("/bucket/object?uploads", "");
    ASSERT_TRUE(init);
    ASSERT_EQ(init->status, 200);
    const auto upload_id = xmlValue(init->body, "UploadId");
    ASSERT_FALSE(upload_id.empty());

    const auto first =
        client.Put("/bucket/object?partNumber=1&uploadId=" + upload_id,
                   "hello ", "application/octet-stream");
    ASSERT_TRUE(first);
    ASSERT_EQ(first->status, 200);

    const auto complete_xml =
        std::string{"<CompleteMultipartUpload>"
                    "<Part><PartNumber>1</PartNumber></Part>"
                    "<Part><PartNumber>2</PartNumber></Part>"
                    "</CompleteMultipartUpload>"};
    const auto missing_part =
        client.Post("/bucket/object?uploadId=" + upload_id, complete_xml);
    ASSERT_TRUE(missing_part);
    EXPECT_EQ(missing_part->status, 403);
    EXPECT_TRUE(received_body.empty());

    const auto second =
        client.Put("/bucket/object?partNumber=2&uploadId=" + upload_id,
                   "world", "application/octet-stream");
    ASSERT_TRUE(second);
    EXPECT_EQ(second->status, 200);

    const auto retry =
        client.Post("/bucket/object?uploadId=" + upload_id, complete_xml);
    ASSERT_TRUE(retry);
    EXPECT_EQ(retry->status, 201);
    EXPECT_EQ(received_body, "hello world");
    EXPECT_EQ(app.getRecord("/bucket/object").deleted,
              minikv::record::Deleted::NO);
  }

  cleanup();
}

TEST(ServerRouteTest, S3MultipartInitRejectsExistingLiveObject) {
  const auto options = appOptions("route-s3-multipart-existing");
  const auto cleanup = [&] {
    std::filesystem::remove_all(options.db_path);
    auto multipart_path = options.db_path;
    multipart_path += ".multipart";
    std::filesystem::remove_all(multipart_path);
  };

  {
    minikv::server::App app{options};
    ASSERT_TRUE(app.putRecord(
        "/bucket/object",
        minikv::record::Record{
            .rvolumes = {"volume-a"},
            .deleted = minikv::record::Deleted::NO,
            .hash = "",
        }));

    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());
    const auto init = client.Post("/bucket/object?uploads", "");

    ASSERT_TRUE(init);
    EXPECT_EQ(init->status, 403);
    EXPECT_TRUE(init->body.empty());
  }

  cleanup();
}

TEST(ServerAppTest, MultipartScratchDirectoryIsCleanedAtStartup) {
  auto options = appOptions("multipart-startup-cleanup");
  auto multipart_path = options.db_path;
  multipart_path += ".multipart";
  const auto cleanup = [&] {
    std::filesystem::remove_all(options.db_path);
    std::filesystem::remove_all(multipart_path);
  };

  std::filesystem::create_directories(multipart_path);
  const auto stale_part = multipart_path / "stale-upload-1";
  {
    auto file = std::ofstream{stale_part};
    file << "stale";
  }
  ASSERT_TRUE(std::filesystem::exists(stale_part));

  {
    minikv::server::App app{options};
    EXPECT_TRUE(std::filesystem::exists(multipart_path));
    EXPECT_FALSE(std::filesystem::exists(stale_part));
  }

  cleanup();
}

TEST(ServerRouteTest, UnlinkRouteSoftDeletesWithoutTouchingRemoteObjects) {
  auto remote_delete_called = false;

  LocalVolumeServer volume;
  volume.server.Delete(R"(/.*)", [&](const minikv::http::Request &,
                                     minikv::http::Response &res) {
    remote_delete_called = true;
    res.status = 204;
  });
  volume.start();

  auto options = appOptions("route-unlink");
  options.volumes = {volume.volume()};
  options.replicas = 1;
  options.subvolumes = 1;
  options.protect = true;
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    ASSERT_TRUE(app.putRecord(
        "/hello",
        minikv::record::Record{
            .rvolumes = {volume.volume()},
            .deleted = minikv::record::Deleted::NO,
            .hash = "321c3cf486ed509164edec1e1981fec8",
        }));

    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());
    const auto res = client.Custom("UNLINK", "/hello");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 204);
    EXPECT_FALSE(remote_delete_called);

    const auto loaded = app.getRecord("/hello");
    EXPECT_EQ(loaded.deleted, minikv::record::Deleted::SOFT);
    EXPECT_EQ(loaded.rvolumes, (std::vector<std::string>{volume.volume()}));
  }

  cleanup();
}

TEST(ServerRouteTest, RebalanceRouteCopiesToTargetAndDeletesStaleReplica) {
  std::string source_path;
  auto source_deleted = false;
  LocalVolumeServer source;
  source.server.Get(R"(/.*)", [&](const minikv::http::Request &req,
                                  minikv::http::Response &res) {
    source_path = req.path;
    res.status = 200;
    res.setContent("payload", "application/octet-stream");
  });
  source.server.Delete(R"(/.*)", [&](const minikv::http::Request &,
                                     minikv::http::Response &res) {
    source_deleted = true;
    res.status = 204;
  });
  source.start();

  std::string target_path;
  std::string target_body;
  LocalVolumeServer target;
  target.server.Get(R"(/.*)", [](const minikv::http::Request &,
                                 minikv::http::Response &res) {
    res.status = 404;
  });
  target.server.Put(R"(/.*)", [&](const minikv::http::Request &req,
                                  minikv::http::Response &res) {
    target_path = req.path;
    target_body = req.body;
    res.status = 201;
  });
  target.start();

  auto options = appOptions("route-rebalance");
  options.volumes = {target.volume()};
  options.replicas = 1;
  options.subvolumes = 1;
  const auto cleanup = [&] { std::filesystem::remove_all(options.db_path); };

  {
    minikv::server::App app{options};
    ASSERT_TRUE(app.putRecord(
        "/hello",
        minikv::record::Record{
            .rvolumes = {source.volume()},
            .deleted = minikv::record::Deleted::NO,
            .hash = "321c3cf486ed509164edec1e1981fec8",
        }));

    LocalVolumeServer master;
    minikv::server::registerRoutes(master.server, app);
    master.start();

    minikv::test::TestClient client("http://" + master.volume());
    const auto res = client.Custom("REBALANCE", "/hello");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 204);

    const auto expected_path = minikv::placement::key2path("/hello");
    EXPECT_EQ(source_path, expected_path);
    EXPECT_EQ(target_path, expected_path);
    EXPECT_EQ(target_body, "payload");
    EXPECT_TRUE(source_deleted);

    const auto loaded = app.getRecord("/hello");
    EXPECT_EQ(loaded.deleted, minikv::record::Deleted::NO);
    EXPECT_EQ(loaded.rvolumes, (std::vector<std::string>{target.volume()}));
  }

  cleanup();
}
