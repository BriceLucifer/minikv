#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace minikv::volume_client {
struct Response {
    int status;
    std::string body;
};

Response remoteGet(std::string_view url);
bool remoteHead(std::string_view url, std::chrono::milliseconds timeout);
void remotePut(std::string_view url, std::string_view body);
void remoteDelete(std::string_view url);

} // namespace minikv::volume_client
