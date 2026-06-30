#include "index.hpp"

#include "leveldb/db.h"
#include "leveldb/options.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace minikv::index {

namespace {

std::string toString(std::string_view value) {
  return std::string{value.data(), value.size()};
}

} // namespace

LevelDbIndex::LevelDbIndex(const std::filesystem::path &path) {
  leveldb::Options options;
  options.create_if_missing = true;

  leveldb::DB *db = nullptr;
  const auto status = leveldb::DB::Open(options, path.string(), &db);

  if (!status.ok()) {
    throw std::runtime_error(status.ToString());
  }

  db_.reset(db);
}

LevelDbIndex::~LevelDbIndex() = default;

record::Record LevelDbIndex::getRecord(std::string_view key) const {
  std::string data;
  const auto status = db_->Get(leveldb::ReadOptions{}, toString(key), &data);

  if (status.IsNotFound()) {
    return record::Record{{}, record::Deleted::HARD, ""};
  }

  if (!status.ok()) {
    throw std::runtime_error(status.ToString());
  }

  return record::toRecord(data);
}

bool LevelDbIndex::putRecord(std::string_view key, const record::Record &rec) {
  const auto data = record::fromRecord(rec);
  const auto status = db_->Put(leveldb::WriteOptions{}, toString(key), data);
  return status.ok();
}

bool LevelDbIndex::deleteRecord(std::string_view key) {
  const auto status = db_->Delete(leveldb::WriteOptions{}, toString(key));
  return status.ok();
}

} // namespace minikv::index
