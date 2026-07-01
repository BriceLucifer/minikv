#pragma once

#include "http.hpp"

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace minikv::test {

struct TestResult {
  http::ClientResponse response;

  explicit operator bool() const { return response.status != 0; }
  const http::ClientResponse *operator->() const { return &response; }
  http::ClientResponse *operator->() { return &response; }
};

class TestClient {
public:
  explicit TestClient(std::string origin) : origin_(std::move(origin)) {}

  [[nodiscard]] TestResult Get(std::string_view path) const {
    return Custom("GET", path);
  }
  [[nodiscard]] TestResult Head(std::string_view path) const {
    return Custom("HEAD", path);
  }
  [[nodiscard]] TestResult Delete(std::string_view path) const {
    return Custom("DELETE", path);
  }

  [[nodiscard]] TestResult Put(std::string_view path, std::string_view body,
                               std::string_view content_type) const {
    return {
        http::request("PUT", origin_ + std::string{path}, body, content_type)};
  }

  [[nodiscard]] TestResult
  Post(std::string_view path, std::string_view body,
       std::string_view content_type = "application/xml") const {
    return {
        http::request("POST", origin_ + std::string{path}, body, content_type)};
  }

  [[nodiscard]] TestResult Custom(std::string_view method,
                                  std::string_view path) const {
    return {http::request(method, origin_ + std::string{path})};
  }

private:
  std::string origin_;
};

class TestHttpServer {
public:
  using RouteHandler =
      std::function<void(const http::Request &, http::Response &)>;

  void Get(std::string pattern, RouteHandler handler) {
    addRoute("GET", std::move(pattern), std::move(handler));
  }

  void Put(std::string pattern, RouteHandler handler) {
    addRoute("PUT", std::move(pattern), std::move(handler));
  }

  void Delete(std::string pattern, RouteHandler handler) {
    addRoute("DELETE", std::move(pattern), std::move(handler));
  }

  void setHandler(http::Handler handler) {
    routes_.clear();
    server_.setHandler(std::move(handler));
    custom_handler_ = true;
  }

  int bindToAnyPort(std::string_view address) {
    installDispatcher();
    return server_.bindToAnyPort(address);
  }

  bool listenAfterBind() { return server_.listenAfterBind(); }
  void waitUntilReady() const { server_.waitUntilReady(); }
  void stop() { server_.stop(); }

  operator http::Server &() {
    custom_handler_ = true;
    return server_;
  }

private:
  struct Route {
    std::string method;
    std::string pattern;
    RouteHandler handler;
  };

  void addRoute(std::string method, std::string pattern, RouteHandler handler) {
    routes_.push_back(
        {std::move(method), std::move(pattern), std::move(handler)});
  }

  void installDispatcher() {
    if (custom_handler_ || routes_.empty()) {
      return;
    }
    server_.setHandler([this](const http::Request &req) {
      auto res = http::Response{};
      res.status = 404;
      for (const auto &route : routes_) {
        const auto method_matches =
            route.method == req.method ||
            (route.method == "GET" && req.method == "HEAD");
        const auto path_matches =
            route.pattern == req.path || route.pattern == R"(/.*)";
        if (!method_matches || !path_matches) {
          continue;
        }
        route.handler(req, res);
        return res;
      }
      return res;
    });
  }

  http::Server server_;
  std::vector<Route> routes_;
  bool custom_handler_ = false;
};

class LocalHttpServer {
public:
  TestHttpServer server;

  void start() {
    port_ = server.bindToAnyPort("127.0.0.1");
    if (port_ < 0) {
      throw std::runtime_error("failed to bind test HTTP server");
    }

    worker_ = std::thread([this] { server.listenAfterBind(); });
    server.waitUntilReady();
  }

  [[nodiscard]] std::string volume() const {
    return "127.0.0.1:" + std::to_string(port_);
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

} // namespace minikv::test
