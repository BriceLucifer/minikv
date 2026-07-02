#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace minikv::volume_client {
struct Response {
  int status;
  std::string body;
};

struct HeadResult {
  bool found;
  std::string content_length;
  std::string etag;
  std::string last_modified;
};

Response remoteGet(std::string_view url);
HeadResult remoteHeadInfo(std::string_view url,
                          std::chrono::milliseconds timeout);
bool remoteHead(std::string_view url, std::chrono::milliseconds timeout);
void remotePut(std::string_view url, std::string_view body);
void remotePutFiles(std::string_view url,
                    const std::vector<std::filesystem::path> &paths,
                    std::uint64_t content_length);
void remoteDelete(std::string_view url);
void clearConnectionCache();

} // namespace minikv::volume_client
