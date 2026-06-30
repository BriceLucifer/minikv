#include "record.hpp"

#include <gtest/gtest.h>

TEST(SmokeTest, GoogleTestIsWired) { EXPECT_EQ(1 + 1, 2); }

namespace {

void expectRoundTrip(const record::Record &rec, std::string_view encoded) {
  EXPECT_EQ(record::fromRecord(rec), encoded);

  const auto decoded = record::toRecord(encoded);
  EXPECT_EQ(decoded.rvolumes, rec.rvolumes);
  EXPECT_EQ(decoded.deleted, rec.deleted);
  EXPECT_EQ(decoded.hash, rec.hash);
}

} // namespace

TEST(RecordTest, EncodeDecodeMatchesGoImplementation) {
  expectRoundTrip(record::Record{{"hello", "world"}, record::Deleted::SOFT, ""},
                  "DELETEDhello,world");
  expectRoundTrip(record::Record{{"hello", "world"}, record::Deleted::NO, ""},
                  "hello,world");
  expectRoundTrip(record::Record{{"hello"}, record::Deleted::NO, ""}, "hello");
  expectRoundTrip(record::Record{{"hello"}, record::Deleted::SOFT, ""},
                  "DELETEDhello");
  expectRoundTrip(record::Record{{"hello"}, record::Deleted::SOFT,
                                 "5d41402abc4b2a76b9719d911017c592"},
                  "DELETEDHASH5d41402abc4b2a76b9719d911017c592hello");
  expectRoundTrip(record::Record{{"hello"}, record::Deleted::NO,
                                 "5d41402abc4b2a76b9719d911017c592"},
                  "HASH5d41402abc4b2a76b9719d911017c592hello");
}

TEST(RecordTest, HardDeleteCannotBeEncoded) {
  EXPECT_THROW(
      record::fromRecord(record::Record{{"hello"}, record::Deleted::HARD, ""}),
      std::runtime_error);
}
