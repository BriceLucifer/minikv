#include "hash.hpp"

#include <gtest/gtest.h>

TEST(HashTest, Md5HexMatchesGoCryptoMd5) {
  EXPECT_EQ(minikv::md5_hex("hello"), "5d41402abc4b2a76b9719d911017c592");
  EXPECT_EQ(minikv::md5_hex("helloworld"), "fc5e038d38a57032085441e7fe7010b0");
}

TEST(HashTest, Md5BytesExposePlacementPrefix) {
  const auto digest = minikv::md5("hello");

  EXPECT_EQ(digest[0], 0x5d);
  EXPECT_EQ(digest[1], 0x41);
}
