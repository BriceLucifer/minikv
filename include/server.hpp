#pragma once

#include "http.hpp"
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

struct RebalanceResult {
  int status;
};

struct S3DeleteResult {
  int status;
};

struct MultipartUploadResult {
  int status;
  std::string upload_id;
  std::string etag;
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
  RebalanceResult rebalanceReplicas(std::string_view key);
  QueryResult query(std::string_view key, std::string_view operation,
                    std::string_view start, std::string_view limit) const;
  QueryResult s3List(std::string_view bucket, std::string_view prefix) const;
  S3DeleteResult s3Delete(std::string_view bucket,
                          const std::vector<std::string> &keys);
  MultipartUploadResult createMultipartUpload(std::string_view key);
  int writeMultipartPart(std::string_view upload_id, int part_number,
                         std::string_view body);
  int abortMultipartUpload(std::string_view upload_id);
  MultipartUploadResult completeMultipartUpload(
      std::string_view key, std::string_view upload_id,
      const std::vector<int> &part_numbers);

  const AppOptions &options() const;

 private:
  std::filesystem::path multipartRoot() const;
  std::filesystem::path multipartPartPath(std::string_view upload_id,
                                          int part_number) const;

  AppOptions options_;
  index::LevelDbIndex index_;
  mutable std::mutex lock_mutex_;
  std::unordered_set<std::string> locks_;
  mutable std::mutex multipart_mutex_;
  std::unordered_set<std::string> upload_ids_;
};

http::Response handleRequest(App &app, const http::Request &request);
void registerRoutes(http::Server &server, App &app);
std::vector<std::size_t> replicaProbeOrder(std::size_t count);
std::vector<std::size_t> replicaProbeOrder(std::size_t count,
                                           std::mt19937 &rng);

} // namespace minikv::server
