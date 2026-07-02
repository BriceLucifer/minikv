#include "http.hpp"
#include "volume_client.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

// Starts an in-process HTTP server for volume_client tests.
// This keeps the tests self-contained while still exercising real HTTP
// requests through the project's Beast adapter.
class LocalHttpServer {
public:
  minikv::http::Server server;

  void setHandler(minikv::http::Handler handler) {
    server.setHandler(std::move(handler));
  }

  void start() {
    port_ = server.bindToAnyPort("127.0.0.1");
    if (port_ < 0) {
      throw std::runtime_error("failed to bind test server");
    }

    worker_ = std::thread([this] { server.listenAfterBind(); });
    server.waitUntilReady();
  }

  [[nodiscard]] std::string url(std::string_view path) const {
    return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
  }

  [[nodiscard]] int port() const { return port_; }

  ~LocalHttpServer() {
    minikv::volume_client::clearConnectionCache();
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

// remote_get(remote) accepts only HTTP 200 and returns the response body.
TEST(VolumeClientTest, RemoteGetReadsBodyFromUrlPath) {
  LocalHttpServer local;
  local.setHandler([](const minikv::http::Request &req) {
    auto res = minikv::http::Response{};
    EXPECT_EQ(req.method, "GET");
    EXPECT_EQ(req.path, "/aa/bb/key");
    res.setContent("stored-value", "application/octet-stream");
    return res;
  });
  local.start();

  const auto res = minikv::volume_client::remoteGet(local.url("/aa/bb/key"));

  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.body, "stored-value");
}

// remote_get(remote) treats any non-200 status as an error.
TEST(VolumeClientTest, RemoteGetRejectsNonOkStatus) {
  LocalHttpServer local;
  local.setHandler([](const minikv::http::Request &) {
    auto res = minikv::http::Response{};
    res.status = 404;
    return res;
  });
  local.start();

  EXPECT_THROW(minikv::volume_client::remoteGet(local.url("/missing")),
               std::runtime_error);
}

// remote_put(remote, length, body) sends the request body and accepts HTTP 201.
TEST(VolumeClientTest, RemotePutSendsBodyAndAcceptsCreated) {
  std::string received_body;

  LocalHttpServer local;
  local.setHandler([&](const minikv::http::Request &req) {
    auto res = minikv::http::Response{};
    res.status = 201;
    EXPECT_EQ(req.method, "PUT");
    EXPECT_EQ(req.path, "/object");
    received_body = req.body;
    return res;
  });
  local.start();

  EXPECT_NO_THROW(
      minikv::volume_client::remotePut(local.url("/object"), "payload"));
  EXPECT_EQ(received_body, "payload");
}

TEST(VolumeClientTest, RemoteRequestsCanReusePersistentConnection) {
  auto request_count = 0;
  LocalHttpServer local;
  local.setHandler([&](const minikv::http::Request &req) {
    auto res = minikv::http::Response{};
    ++request_count;
    if (req.method == "HEAD") {
      res.setHeader("Content-Length", "7");
      return res;
    }
    if (req.method == "PUT") {
      EXPECT_EQ(req.body, "payload");
      res.status = 201;
      return res;
    }
    if (req.method == "DELETE") {
      res.status = 204;
      return res;
    }
    res.status = 500;
    return res;
  });
  local.start();

  const auto head = minikv::volume_client::remoteHeadInfo(
      local.url("/object"), std::chrono::milliseconds{250});
  EXPECT_TRUE(head.found);
  EXPECT_EQ(head.content_length, "7");
  EXPECT_NO_THROW(
      minikv::volume_client::remotePut(local.url("/object"), "payload"));
  EXPECT_NO_THROW(minikv::volume_client::remoteDelete(local.url("/object")));
  EXPECT_EQ(request_count, 3);
}

// remote_put(remote, length, body) also accepts HTTP 204.
TEST(VolumeClientTest, RemotePutAcceptsNoContent) {
  LocalHttpServer local;
  local.setHandler([](const minikv::http::Request &req) {
    EXPECT_EQ(req.method, "PUT");
    EXPECT_EQ(req.path, "/object");
    auto res = minikv::http::Response{};
    res.status = 204;
    return res;
  });
  local.start();

  EXPECT_NO_THROW(
      minikv::volume_client::remotePut(local.url("/object"), "payload"));
}

TEST(VolumeClientTest, RemotePutFilesStreamsFilesAsOneRequestBody) {
  std::string received_body;

  LocalHttpServer local;
  local.setHandler([&](const minikv::http::Request &req) {
    auto res = minikv::http::Response{};
    res.status = 201;
    EXPECT_EQ(req.method, "PUT");
    EXPECT_EQ(req.path, "/multipart");
    received_body = req.body;
    return res;
  });
  local.start();

  const auto root =
      std::filesystem::temp_directory_path() / "minikv-remote-put-files-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto first = root / "first";
  const auto second = root / "second";
  {
    auto out = std::ofstream{first, std::ios::binary};
    out << "hello ";
  }
  {
    auto out = std::ofstream{second, std::ios::binary};
    out << "world";
  }

  EXPECT_NO_THROW(minikv::volume_client::remotePutFiles(local.url("/multipart"),
                                                        {first, second}, 11));
  EXPECT_EQ(received_body, "hello world");

  std::filesystem::remove_all(root);
}

// remote_delete(remote) accepts both HTTP 204 and HTTP 404, making delete
// idempotent when the remote object is already missing.
TEST(VolumeClientTest, RemoteDeleteAcceptsNoContentAndNotFound) {
  LocalHttpServer local;
  local.setHandler([](const minikv::http::Request &req) {
    EXPECT_EQ(req.method, "DELETE");
    if (req.path == "/present") {
      auto res = minikv::http::Response{};
      res.status = 204;
      return res;
    }
    if (req.path == "/missing") {
      auto res = minikv::http::Response{};
      res.status = 404;
      return res;
    }
    auto res = minikv::http::Response{};
    res.status = 500;
    return res;
  });
  local.start();

  EXPECT_NO_THROW(minikv::volume_client::remoteDelete(local.url("/present")));
  EXPECT_NO_THROW(minikv::volume_client::remoteDelete(local.url("/missing")));
}

// remote_head(remote, timeout) returns true for HTTP 200 and false for other
// reachable statuses.
TEST(VolumeClientTest, RemoteHeadReturnsWhetherObjectExists) {
  LocalHttpServer local;
  local.setHandler([](const minikv::http::Request &req) {
    EXPECT_EQ(req.method, "HEAD");
    if (req.path == "/present") {
      auto res = minikv::http::Response{};
      res.status = 200;
      return res;
    }
    if (req.path == "/missing") {
      auto res = minikv::http::Response{};
      res.status = 404;
      return res;
    }
    auto res = minikv::http::Response{};
    res.status = 500;
    return res;
  });
  local.start();

  using namespace std::chrono_literals;
  EXPECT_TRUE(minikv::volume_client::remoteHead(local.url("/present"), 100ms));
  EXPECT_FALSE(minikv::volume_client::remoteHead(local.url("/missing"), 100ms));
}

TEST(VolumeClientTest, RemoteHeadInfoReturnsObjectMetadata) {
  LocalHttpServer local;
  local.setHandler([](const minikv::http::Request &req) {
    auto res = minikv::http::Response{};
    res.status = 200;
    EXPECT_EQ(req.method, "HEAD");
    EXPECT_EQ(req.path, "/present");
    res.setHeader("Content-Length", "123");
    res.setHeader("ETag", "\"abc123\"");
    res.setHeader("Last-Modified", "Wed, 01 Jul 2026 02:00:00 GMT");
    return res;
  });
  local.start();

  using namespace std::chrono_literals;
  const auto head =
      minikv::volume_client::remoteHeadInfo(local.url("/present"), 100ms);

  EXPECT_TRUE(head.found);
  EXPECT_EQ(head.content_length, "123");
  EXPECT_EQ(head.etag, "\"abc123\"");
  EXPECT_EQ(head.last_modified, "Wed, 01 Jul 2026 02:00:00 GMT");
}

// Real nginx HEAD responses include the object's Content-Length while sending
// no response body. The adapter must not wait for a body that will never
// arrive.
TEST(HttpAdapterTest, HeadSkipsNonZeroLengthResponseBody) {
  LocalHttpServer local;
  local.setHandler([](const minikv::http::Request &req) {
    auto res = minikv::http::Response{};
    EXPECT_EQ(req.method, "HEAD");
    EXPECT_EQ(req.path, "/object");
    res.setHeader("Content-Length", "12345");
    res.setHeader("Etag", "abc");
    return res;
  });
  local.start();

  const auto res = minikv::http::request("HEAD", local.url("/object"));

  EXPECT_EQ(res.status, 200);
  EXPECT_TRUE(res.body.empty());
  EXPECT_EQ(res.headerValue("Content-Length"), "12345");
  EXPECT_EQ(res.headerValue("etag"), "abc");
}

TEST(HttpAdapterTest, PercentDecodesPathAndQueryParameters) {
  LocalHttpServer local;
  local.setHandler([](const minikv::http::Request &req) {
    auto res = minikv::http::Response{};
    EXPECT_EQ(req.method, "GET");
    EXPECT_EQ(req.target,
              "/bucket/a%20b%2Fc?prefix=hello%20world&empty&plus=a+b");
    EXPECT_EQ(req.path, "/bucket/a b/c");
    EXPECT_EQ(req.getParamValue("prefix"), "hello world");
    EXPECT_TRUE(req.hasParam("empty"));
    EXPECT_EQ(req.getParamValue("empty"), "");
    EXPECT_EQ(req.getParamValue("plus"), "a b");
    res.setContent("ok", "text/plain");
    return res;
  });
  local.start();

  const auto res = minikv::http::request(
      "GET",
      local.url("/bucket/a%20b%2Fc?prefix=hello%20world&empty&plus=a+b"));

  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.body, "ok");
}

TEST(HttpAdapterTest, DuplicateQueryKeysKeepFirstValueLikeGo) {
  LocalHttpServer local;
  local.setHandler([](const minikv::http::Request &req) {
    auto res = minikv::http::Response{};
    EXPECT_EQ(req.path, "/bucket");
    EXPECT_EQ(req.getParamValue("prefix"), "first");
    res.setContent("ok", "text/plain");
    return res;
  });
  local.start();

  const auto res = minikv::http::request(
      "GET", local.url("/bucket?prefix=first&prefix=second"));

  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.body, "ok");
}

TEST(HttpAdapterTest, MalformedPercentEscapesRemainLiteral) {
  LocalHttpServer local;
  local.setHandler([](const minikv::http::Request &req) {
    auto res = minikv::http::Response{};
    EXPECT_EQ(req.path, "/bad%ZZ/path%");
    EXPECT_EQ(req.getParamValue("key"), "value%QQ");
    res.setContent("ok", "text/plain");
    return res;
  });
  local.start();

  const auto res =
      minikv::http::request("GET", local.url("/bad%ZZ/path%?key=value%QQ"));

  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.body, "ok");
}

TEST(HttpAdapterTest, ClientReadTimeoutThrowsForStalledResponse) {
  LocalHttpServer local;
  local.setHandler([](const minikv::http::Request &) {
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    auto res = minikv::http::Response{};
    res.setContent("too late", "text/plain");
    return res;
  });
  local.start();

  EXPECT_THROW(
      [&local] {
        auto response = minikv::http::request("GET", local.url("/slow"), {},
                                              "application/octet-stream",
                                              std::chrono::milliseconds{20});
        EXPECT_EQ(response.status, 200);
      }(),
      std::exception);
}

TEST(HttpAdapterTest, CustomMethodsReachServerHandler) {
  LocalHttpServer local;
  local.setHandler([](const minikv::http::Request &req) {
    auto res = minikv::http::Response{};
    EXPECT_EQ(req.method, "REBALANCE");
    EXPECT_EQ(req.path, "/object");
    res.status = 204;
    return res;
  });
  local.start();

  const auto res = minikv::http::request("REBALANCE", local.url("/object"));

  EXPECT_EQ(res.status, 204);
}

TEST(HttpAdapterTest, KeepsHttp11ConnectionsAliveForMultipleRequests) {
  auto requests = 0;
  LocalHttpServer local;
  local.setHandler([&](const minikv::http::Request &req) {
    auto res = minikv::http::Response{};
    ++requests;
    res.setContent(req.path, "text/plain");
    return res;
  });
  local.start();

  namespace asio = boost::asio;
  namespace beast = boost::beast;
  namespace bhttp = boost::beast::http;
  using tcp = asio::ip::tcp;

  auto ioc = asio::io_context{};
  auto socket = tcp::socket{ioc};
  asio::connect(socket, tcp::resolver{ioc}.resolve(
                            "127.0.0.1", std::to_string(local.port())));
  auto buffer = beast::flat_buffer{};

  for (const auto *path : {"/first", "/second"}) {
    auto req = bhttp::request<bhttp::string_body>{bhttp::verb::get, path, 11};
    req.set(bhttp::field::host, "127.0.0.1");
    req.keep_alive(true);
    bhttp::write(socket, req);

    auto res = bhttp::response<bhttp::string_body>{};
    bhttp::read(socket, buffer, res);
    EXPECT_EQ(res.result_int(), 200U);
    EXPECT_TRUE(res.keep_alive());
    EXPECT_EQ(res.body(), path);
  }

  auto ec = boost::system::error_code{};
  socket.shutdown(tcp::socket::shutdown_both, ec);
  EXPECT_EQ(requests, 2);
}
