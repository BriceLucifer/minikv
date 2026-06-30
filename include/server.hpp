#pragma once

#include "index.hpp"
#include "record.hpp"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace minikv::server {

struct AppOptions {
  std::filesystem::path db_path;
  std::vector<std::string> volumes;
  std::string fallback;
  int replicas = 3;
  int subvolumes = 10;
  bool protect = false;
  bool md5sum = true;
  std::chrono::milliseconds volume_timeout{1000};
};

class App {
 public:
  explicit App(AppOptions options);

  bool lockKey(std::string_view key);
  void unlockKey(std::string_view key);

  record::Record getRecord(std::string_view key) const;
  bool putRecord(std::string_view key, const record::Record &rec);
  bool deleteRecord(std::string_view key);

  const AppOptions &options() const;

 private:
  AppOptions options_;
  index::LevelDbIndex index_;
  mutable std::mutex lock_mutex_;
  std::unordered_set<std::string> locks_;
};

} // namespace minikv::server
