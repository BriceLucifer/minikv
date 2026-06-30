#include "volume_client.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

// Starts an in-process HTTP server for volume_client tests.
// This keeps the tests self-contained while still exercising real HTTP
// requests through cpp-httplib.
class LocalHttpServer {
public:
    httplib::Server server;

    void start() {
        port_ = server.bind_to_any_port("127.0.0.1");
        if (port_ < 0) {
            throw std::runtime_error("failed to bind test server");
        }

        worker_ = std::thread([this] {
            server.listen_after_bind();
        });
        server.wait_until_ready();
    }

    std::string url(std::string_view path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }

    ~LocalHttpServer() {
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
    local.server.Get("/aa/bb/key", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("stored-value", "application/octet-stream");
    });
    local.start();

    const auto res = minikv::volume_client::remoteGet(local.url("/aa/bb/key"));

    EXPECT_EQ(res.status, 200);
    EXPECT_EQ(res.body, "stored-value");
}

// remote_get(remote) treats any non-200 status as an error.
TEST(VolumeClientTest, RemoteGetRejectsNonOkStatus) {
    LocalHttpServer local;
    local.server.Get("/missing", [](const httplib::Request&, httplib::Response& res) {
        res.status = 404;
    });
    local.start();

    EXPECT_THROW(
        minikv::volume_client::remoteGet(local.url("/missing")),
        std::runtime_error
    );
}

// remote_put(remote, length, body) sends the request body and accepts HTTP 201.
TEST(VolumeClientTest, RemotePutSendsBodyAndAcceptsCreated) {
    std::string received_body;

    LocalHttpServer local;
    local.server.Put("/object", [&](const httplib::Request& req, httplib::Response& res) {
        received_body = req.body;
        res.status = 201;
    });
    local.start();

    EXPECT_NO_THROW(minikv::volume_client::remotePut(local.url("/object"), "payload"));
    EXPECT_EQ(received_body, "payload");
}

// remote_put(remote, length, body) also accepts HTTP 204.
TEST(VolumeClientTest, RemotePutAcceptsNoContent) {
    LocalHttpServer local;
    local.server.Put("/object", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });
    local.start();

    EXPECT_NO_THROW(minikv::volume_client::remotePut(local.url("/object"), "payload"));
}

// remote_delete(remote) accepts both HTTP 204 and HTTP 404, making delete
// idempotent when the remote object is already missing.
TEST(VolumeClientTest, RemoteDeleteAcceptsNoContentAndNotFound) {
    LocalHttpServer local;
    local.server.Delete("/present", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });
    local.server.Delete("/missing", [](const httplib::Request&, httplib::Response& res) {
        res.status = 404;
    });
    local.start();

    EXPECT_NO_THROW(minikv::volume_client::remoteDelete(local.url("/present")));
    EXPECT_NO_THROW(minikv::volume_client::remoteDelete(local.url("/missing")));
}

// remote_head(remote, timeout) returns true for HTTP 200 and false for other
// reachable statuses.
TEST(VolumeClientTest, RemoteHeadReturnsWhetherObjectExists) {
    LocalHttpServer local;
    local.server.Get("/present", [](const httplib::Request&, httplib::Response& res) {
        res.status = 200;
    });
    local.server.Get("/missing", [](const httplib::Request&, httplib::Response& res) {
        res.status = 404;
    });
    local.start();

    using namespace std::chrono_literals;
    EXPECT_TRUE(minikv::volume_client::remoteHead(local.url("/present"), 100ms));
    EXPECT_FALSE(minikv::volume_client::remoteHead(local.url("/missing"), 100ms));
}
