#include "volume_client.hpp"

#include "http.hpp"

#include <chrono>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

namespace minikv::volume_client {

namespace {
void requireStatus(std::string_view op, int actual, std::initializer_list<int> expected) {
    for (const auto status : expected) {
        if (actual == status) {
            return;
        }
    }

    throw std::runtime_error(
        std::string(op) + ": wrong status code " + std::to_string(actual)
    );
}

} // namespace

Response remoteGet(std::string_view url) {
    const auto res = http::request("GET", url);
    requireStatus("remote_get", res.status, {200});
    return {res.status, res.body};
}

bool remoteHead(std::string_view url, std::chrono::milliseconds timeout) {
    const auto res = http::request("HEAD", url, {}, "application/octet-stream", timeout);
    return res.status == 200;
}

void remotePut(std::string_view url, std::string_view body) {
    const auto res = http::request("PUT", url, body);
    requireStatus("remote_put", res.status, {201, 204});
}

void remoteDelete(std::string_view remote) {
    const auto res = http::request("DELETE", remote);
    requireStatus("remote_delete", res.status, {204, 404});
}

} // namespace minikv::volume_client
