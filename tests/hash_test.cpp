#include "hash.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

TEST(HashTest, Md5HexMatchesGoCryptoMd5) {
  EXPECT_EQ(minikv::md5_hex("hello"), "5d41402abc4b2a76b9719d911017c592");
  EXPECT_EQ(minikv::md5_hex("helloworld"), "fc5e038d38a57032085441e7fe7010b0");
}

TEST(HashTest, Md5BytesExposePlacementPrefix) {
  const auto digest = minikv::md5("hello");

  EXPECT_EQ(digest[0], 0x5d);
  EXPECT_EQ(digest[1], 0x41);
}

TEST(HashTest, Md5HexFilesStreamsMultipleFiles) {
  const auto root = std::filesystem::temp_directory_path() /
                    "minikv-md5-files-test";
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

  EXPECT_EQ(minikv::md5_hex_files({first, second}),
            minikv::md5_hex("hello world"));

  std::filesystem::remove_all(root);
}
