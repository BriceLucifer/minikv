#include "http.hpp"

#include <boost/json.hpp>

#include <gtest/gtest.h>

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
