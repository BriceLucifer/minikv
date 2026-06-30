#include "base64.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

TEST(Base64Test, EncodesLikeGoStdEncoding) {
  EXPECT_EQ(minikv::base64_encode(""), "");
  EXPECT_EQ(minikv::base64_encode("hello"), "aGVsbG8=");
  EXPECT_EQ(minikv::base64_encode("helloworld"), "aGVsbG93b3JsZA==");
}

TEST(Base64Test, DecodesLikeGoStdEncoding) {
  EXPECT_EQ(minikv::base64_decode(""), "");
  EXPECT_EQ(minikv::base64_decode("aGVsbG8="), "hello");
  EXPECT_EQ(minikv::base64_decode("aGVsbG93b3JsZA=="), "helloworld");
}

TEST(Base64Test, DecodeRejectsInvalidInput) {
  EXPECT_THROW(minikv::base64_decode("abc"), std::invalid_argument);
  EXPECT_THROW(minikv::base64_decode("ab=c"), std::invalid_argument);
  EXPECT_THROW(minikv::base64_decode("!!!!"), std::invalid_argument);
}
