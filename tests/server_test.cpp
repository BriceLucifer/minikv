#include "server.hpp"

#include "hash.hpp"
#include "placement.hpp"
#include "record.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <filesystem>
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

class LocalVolumeServer {
public:
  httplib::Server server;

  void start() {
    port_ = server.bind_to_any_port("127.0.0.1");
    if (port_ < 0) {
      throw std::runtime_error("failed to bind test volume server");
    }

    worker_ = std::thread([this] {
      server.listen_after_bind();
    });
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
  volume.server.Put(R"(/.*)", [&](const httplib::Request &req,
                                  httplib::Response &res) {
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
  volume.server.Get(R"(/.*)", [](const httplib::Request &,
                                 httplib::Response &res) {
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
  volume.server.Delete(R"(/.*)", [&](const httplib::Request &req,
                                     httplib::Response &res) {
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
  volume.server.Delete(R"(/.*)", [](const httplib::Request &,
                                    httplib::Response &res) {
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
  volume.server.Delete(R"(/.*)", [&](const httplib::Request &,
                                     httplib::Response &res) {
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
  volume.server.Put(R"(/.*)", [&](const httplib::Request &req,
                                  httplib::Response &res) {
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

    httplib::Client client("http://" + master.volume());
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

TEST(ServerRouteTest, GetAndHeadRoutesReturnRedirectLocation) {
  LocalVolumeServer volume;
  volume.server.Get(R"(/.*)", [](const httplib::Request &,
                                 httplib::Response &res) {
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

    httplib::Client client("http://" + master.volume());
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

TEST(ServerRouteTest, DeleteRouteDeletesRemoteObjects) {
  auto remote_delete_called = false;

  LocalVolumeServer volume;
  volume.server.Delete(R"(/.*)", [&](const httplib::Request &,
                                     httplib::Response &res) {
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

    httplib::Client client("http://" + master.volume());
    const auto res = client.Delete("/hello");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 204);
    EXPECT_TRUE(remote_delete_called);
    EXPECT_EQ(app.getRecord("/hello").deleted, minikv::record::Deleted::HARD);
  }

  cleanup();
}
