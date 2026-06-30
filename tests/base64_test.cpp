#include "base64.hpp"

#include <gtest/gtest.h>

TEST(Base64Test, EncodesLikeGoStdEncoding) {
  EXPECT_EQ(minikv::base64_encode(""), "");
  EXPECT_EQ(minikv::base64_encode("hello"), "aGVsbG8=");
  EXPECT_EQ(minikv::base64_encode("helloworld"), "aGVsbG93b3JsZA==");
}
