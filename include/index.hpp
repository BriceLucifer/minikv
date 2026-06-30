#pragma once

#include "record.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace leveldb {
class DB;
}

namespace minikv::index {

struct ListedRecord {
  std::string key;
  record::Record record;
};

struct ListRecordsResult {
  std::string next;
  std::vector<ListedRecord> records;
};

class LevelDbIndex {
 public:
  explicit LevelDbIndex(const std::filesystem::path &path);
  ~LevelDbIndex();

  LevelDbIndex(const LevelDbIndex &) = delete;
  LevelDbIndex &operator=(const LevelDbIndex &) = delete;
  LevelDbIndex(LevelDbIndex &&) noexcept = default;
  LevelDbIndex &operator=(LevelDbIndex &&) noexcept = default;

  record::Record getRecord(std::string_view key) const;
  bool putRecord(std::string_view key, const record::Record &rec);
  bool deleteRecord(std::string_view key);
  bool clear();
  ListRecordsResult listRecords(std::string_view prefix, std::string_view start,
                                std::size_t limit) const;

 private:
  std::unique_ptr<leveldb::DB> db_;
};

} // namespace minikv::index
