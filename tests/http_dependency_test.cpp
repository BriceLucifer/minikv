#include "http.hpp"
#include "http_test_util.hpp"

#include <boost/json.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

TEST(HttpDependencyTest, AdapterResponseHeadersAreAvailable) {
  auto response = minikv::http::Response{};
  response.setHeader("Content-Type", "text/plain");

  const auto it = response.headers.find("Content-Type");
  ASSERT_NE(it, response.headers.end());
  EXPECT_EQ(it->second, "text/plain");
}

TEST(JsonDependencyTest, BoostJsonLibraryIsAvailable) {
  auto body = boost::json::object{};
  body["name"] = "minikv";
  body["replicas"] = 3;

  EXPECT_EQ(body.at("name").as_string(), "minikv");
  EXPECT_EQ(body.at("replicas").as_int64(), 3);
}

TEST(HttpAdapterTest, AcceptsLargeRequestBodies) {
  minikv::test::LocalHttpServer server;
  server.server.setHandler([](const minikv::http::Request &req) {
    auto res = minikv::http::Response{};
    res.status = 200;
    res.setContent(std::to_string(req.body.size()), "text/plain");
    return res;
  });
  server.start();

  const auto body = std::string(std::size_t{2} * 1024 * 1024, 'x');
  const auto res =
      minikv::http::request("PUT", "http://" + server.volume() + "/large", body,
                            "application/octet-stream");

  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.body, std::to_string(body.size()));
}

TEST(HttpAdapterTest, RejectsRequestBodiesOverConfiguredLimit) {
  minikv::test::LocalHttpServer server;
  server.server.setBodyLimit(4);
  server.server.setHandler([](const minikv::http::Request &) {
    auto res = minikv::http::Response{};
    res.status = 200;
    return res;
  });
  server.start();

  const auto res =
      minikv::http::request("PUT", "http://" + server.volume() + "/too-large",
                            "12345", "application/octet-stream");

  EXPECT_EQ(res.status, 413);
  EXPECT_TRUE(res.body.empty());
}
