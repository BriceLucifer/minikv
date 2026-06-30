#pragma once

#include "record.hpp"

#include <filesystem>
#include <memory>
#include <string_view>

namespace leveldb {
class DB;
}

namespace minikv::index {

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

 private:
  std::unique_ptr<leveldb::DB> db_;
};

} // namespace minikv::index
