#pragma once

#include "index.hpp"
#include "record.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <random>
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
  std::string content_md5;
  std::string key_volumes;
  std::string key_balance;
};

struct DeleteResult {
  int status;
  record::Record record;
};

struct QueryResult {
  int status;
  std::string content_type;
  std::string body;
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
  QueryResult query(std::string_view key, std::string_view operation,
                    std::string_view start, std::string_view limit) const;

  const AppOptions &options() const;

 private:
  AppOptions options_;
  index::LevelDbIndex index_;
  mutable std::mutex lock_mutex_;
  std::unordered_set<std::string> locks_;
};

void registerRoutes(httplib::Server &server, App &app);
std::vector<std::size_t> replicaProbeOrder(std::size_t count);
std::vector<std::size_t> replicaProbeOrder(std::size_t count,
                                           std::mt19937 &rng);

} // namespace minikv::server
