#include "index.hpp"

#include "leveldb/db.h"
#include "leveldb/options.h"

#include <cstddef>
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

ListRecordsResult LevelDbIndex::listRecords(std::string_view prefix,
                                            std::string_view start,
                                            std::size_t limit) const {
  auto result = ListRecordsResult{};
  auto it = std::unique_ptr<leveldb::Iterator>{
      db_->NewIterator(leveldb::ReadOptions{})};

  const auto prefix_string = toString(prefix);
  const auto seek_key = start.empty() ? prefix_string : toString(start);
  for (it->Seek(seek_key); it->Valid(); it->Next()) {
    const auto key = it->key().ToString();
    if (!key.starts_with(prefix_string)) {
      break;
    }

    if (limit > 0 && result.records.size() == limit) {
      result.next = key;
      break;
    }

    result.records.push_back(ListedRecord{
        .key = key,
        .record = record::toRecord(it->value().ToString()),
    });
  }

  if (!it->status().ok()) {
    throw std::runtime_error(it->status().ToString());
  }

  return result;
}

} // namespace minikv::index
