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

namespace httplib {
class Server;
} // namespace httplib

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

struct WriteResult {
  int status;
  record::Record record;
};

struct ReadResult {
  int status;
  std::string redirect_url;
  record::Record record;
};

struct DeleteResult {
  int status;
  record::Record record;
};

class App {
 public:
  explicit App(AppOptions options);

  bool lockKey(std::string_view key);
  void unlockKey(std::string_view key);

  record::Record getRecord(std::string_view key) const;
  bool putRecord(std::string_view key, const record::Record &rec);
  bool deleteRecord(std::string_view key);

  WriteResult writeToReplicas(std::string_view key, std::string_view value);
  ReadResult readFromReplica(std::string_view key);
  DeleteResult deleteFromReplicas(std::string_view key, bool unlink = false);

  const AppOptions &options() const;

 private:
  AppOptions options_;
  index::LevelDbIndex index_;
  mutable std::mutex lock_mutex_;
  std::unordered_set<std::string> locks_;
};

void registerRoutes(httplib::Server &server, App &app);

} // namespace minikv::server
