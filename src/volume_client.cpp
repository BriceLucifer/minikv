#include "volume_client.hpp"

#include <httplib.h>

#include <chrono>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

namespace minikv::volume_client {

namespace {
struct RemoteTarget {
    std::string origin;
    std::string path;
};

RemoteTarget parseUrl(std::string_view remote) {
    const auto scheme_end = remote.find("://");
    if (scheme_end == std::string_view::npos) {
        throw std::invalid_argument("remote url requires scheme");
    }

    const auto path_start = remote.find('/', scheme_end + 3);
    if (path_start == std::string_view::npos) {
        return {std::string(remote), "/"};
    }

    return {
        std::string(remote.substr(0, path_start)),
        std::string(remote.substr(path_start)),
    };
}

httplib::Client makeClient(const RemoteTarget& target) {
    return httplib::Client(target.origin);
}

void throwRequestError(std::string_view op, httplib::Error error) {
    throw std::runtime_error(
        std::string(op) + ": request failed: " + httplib::to_string(error)
    );
}

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
    const auto target = parseUrl(url);
    auto client = makeClient(target);

    auto res = client.Get(target.path);
    if (!res) {
        throwRequestError("remote_get", res.error());
    }

    requireStatus("remote_get", res->status, {200});
    return {res->status, res->body};
}

bool remoteHead(std::string_view url, std::chrono::milliseconds timeout) {
    const auto target = parseUrl(url);
    auto client = makeClient(target);

    client.set_connection_timeout(timeout);
    client.set_read_timeout(timeout);
    client.set_write_timeout(timeout);

    auto res = client.Head(target.path);
    if (!res) {
        throwRequestError("remote_head", res.error());
    }

    return res->status == 200;
}

void remotePut(std::string_view url, std::string_view body) {
    const auto target = parseUrl(url);
    auto client = makeClient(target);

    auto res = client.Put(
        target.path,
        body.data(),
        body.size(),
        "application/octet-stream"
    );

    if (!res) {
        throwRequestError("remote_put", res.error());
    }

    requireStatus("remote_put", res->status, {201, 204});
}

void remoteDelete(std::string_view remote) {
    const auto target = parseUrl(remote);
    auto client = makeClient(target);

    auto res = client.Delete(target.path);
    if (!res) {
        throwRequestError("remote_delete", res.error());
    }

    requireStatus("remote_delete", res->status, {204, 404});
}

} // namespace minikv::volume_client
