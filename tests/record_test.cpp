#include "record.hpp"

#include <gtest/gtest.h>

TEST(SmokeTest, GoogleTestIsWired) { EXPECT_EQ(1 + 1, 2); }

namespace {

void expectRoundTrip(const minikv::record::Record &rec,
                     std::string_view encoded) {
  EXPECT_EQ(minikv::record::fromRecord(rec), encoded);

  const auto decoded = minikv::record::toRecord(encoded);
  EXPECT_EQ(decoded.rvolumes, rec.rvolumes);
  EXPECT_EQ(decoded.deleted, rec.deleted);
  EXPECT_EQ(decoded.hash, rec.hash);
}

} // namespace

TEST(RecordTest, EncodeDecodeMatchesGoImplementation) {
  expectRoundTrip(minikv::record::Record{{"hello", "world"},
                                         minikv::record::Deleted::SOFT, ""},
                  "DELETEDhello,world");
  expectRoundTrip(minikv::record::Record{{"hello", "world"},
                                         minikv::record::Deleted::NO, ""},
                  "hello,world");
  expectRoundTrip(minikv::record::Record{{"hello"}, minikv::record::Deleted::NO,
                                         ""},
                  "hello");
  expectRoundTrip(minikv::record::Record{{"hello"},
                                         minikv::record::Deleted::SOFT, ""},
                  "DELETEDhello");
  expectRoundTrip(minikv::record::Record{
                      {"hello"}, minikv::record::Deleted::SOFT,
                      "5d41402abc4b2a76b9719d911017c592"},
                  "DELETEDHASH5d41402abc4b2a76b9719d911017c592hello");
  expectRoundTrip(minikv::record::Record{
                      {"hello"}, minikv::record::Deleted::NO,
                      "5d41402abc4b2a76b9719d911017c592"},
                  "HASH5d41402abc4b2a76b9719d911017c592hello");
}

TEST(RecordTest, HardDeleteCannotBeEncoded) {
  EXPECT_THROW(
      minikv::record::fromRecord(minikv::record::Record{
          {"hello"}, minikv::record::Deleted::HARD, ""}),
      std::runtime_error);
}
