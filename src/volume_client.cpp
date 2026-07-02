#include "volume_client.hpp"

#include "http.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace minikv::volume_client {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = boost::beast::http;
using tcp = asio::ip::tcp;

namespace {
struct ParsedUrl {
  std::string host;
  std::string port;
  std::string target;
};

std::string toString(std::string_view value) {
  return std::string{value.data(), value.size()};
}

std::string lowercase(std::string_view value) {
  auto out = toString(value);
  std::ranges::transform(out, out.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return out;
}

ParsedUrl parseHttpUrl(std::string_view url) {
  static constexpr auto scheme = std::string_view{"http://"};
  if (!url.starts_with(scheme)) {
    throw std::invalid_argument("only http:// URLs are supported");
  }
  url.remove_prefix(scheme.size());

  const auto slash = url.find('/');
  const auto authority =
      slash == std::string_view::npos ? url : url.substr(0, slash);
  auto target = slash == std::string_view::npos ? std::string{"/"}
                                                : toString(url.substr(slash));

  const auto colon = authority.rfind(':');
  auto host = std::string{};
  auto port = std::string{"80"};
  if (colon == std::string_view::npos) {
    host = toString(authority);
  } else {
    host = toString(authority.substr(0, colon));
    port = toString(authority.substr(colon + 1));
  }

  return {.host = host, .port = port, .target = target};
}

std::string connectionKey(const ParsedUrl &url) {
  return url.host + ":" + url.port;
}

template <typename Future, typename Cancel>
void waitForOperation(asio::io_context &ioc, Future &future,
                      std::chrono::milliseconds timeout, Cancel cancel) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (future.wait_for(std::chrono::milliseconds{0}) !=
         std::future_status::ready) {
    ioc.restart();
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      cancel();
      ioc.restart();
      ioc.run();
      throw std::runtime_error("volume http request timed out");
    }
    ioc.run_for(deadline - now);
  }
}

void requireStatus(std::string_view op, int actual,
                   std::initializer_list<int> expected) {
  for (const auto status : expected) {
    if (actual == status) {
      return;
    }
  }

  throw std::runtime_error(std::string(op) + ": wrong status code " +
                           std::to_string(actual));
}

class PooledConnection {
public:
  PooledConnection(std::string host, std::string port)
      : host_(std::move(host)), port_(std::move(port)), resolver_(ioc_),
        stream_(ioc_) {}

  http::ClientResponse request(std::string_view method, std::string_view target,
                               std::string_view body,
                               std::chrono::milliseconds timeout) {
    auto lock = std::scoped_lock{mutex_};
    try {
      return requestOnce(method, target, body, timeout);
    } catch (const std::exception &) {
      closeUnlocked();
      return requestOnce(method, target, body, timeout);
    }
  }

  void close() {
    auto lock = std::scoped_lock{mutex_};
    closeUnlocked();
  }

private:
  void closeUnlocked() {
    auto ec = boost::system::error_code{};
    stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
    stream_.socket().close(ec);
    buffer_.consume(buffer_.size());
    connected_ = false;
  }

  void connect(std::chrono::milliseconds timeout) {
    if (connected_ && stream_.socket().is_open()) {
      return;
    }

    buffer_.consume(buffer_.size());
    ioc_.restart();
    auto resolve_future =
        resolver_.async_resolve(host_, port_, asio::use_future);
    waitForOperation(ioc_, resolve_future, timeout,
                     [this] { resolver_.cancel(); });
    const auto endpoints = resolve_future.get();

    stream_.expires_after(timeout);
    auto connect_future = stream_.async_connect(endpoints, asio::use_future);
    waitForOperation(ioc_, connect_future, timeout,
                     [this] { stream_.cancel(); });
    [[maybe_unused]] const auto connected_endpoint = connect_future.get();
    connected_ = true;
  }

  http::ClientResponse requestOnce(std::string_view method,
                                   std::string_view target,
                                   std::string_view body,
                                   std::chrono::milliseconds timeout) {
    connect(timeout);

    auto req = bhttp::request<bhttp::string_body>{};
    req.version(11);
    req.method_string(method);
    req.target(target);
    req.set(bhttp::field::host, host_ + ":" + port_);
    req.set(bhttp::field::user_agent, "minikv-volume-client");
    req.keep_alive(true);
    if (!body.empty() || method == "PUT" || method == "POST") {
      req.set(bhttp::field::content_type, "application/octet-stream");
      req.body() = toString(body);
    }
    req.prepare_payload();

    stream_.expires_after(timeout);
    auto write_future = bhttp::async_write(stream_, req, asio::use_future);
    waitForOperation(ioc_, write_future, timeout, [this] { stream_.cancel(); });
    [[maybe_unused]] const auto bytes_written = write_future.get();

    auto out = http::ClientResponse{};
    if (lowercase(method) == "head") {
      auto parser = bhttp::response_parser<bhttp::empty_body>{};
      parser.skip(true);
      stream_.expires_after(timeout);
      auto read_future =
          bhttp::async_read(stream_, buffer_, parser, asio::use_future);
      waitForOperation(ioc_, read_future, timeout,
                       [this] { stream_.cancel(); });
      [[maybe_unused]] const auto bytes_read = read_future.get();
      const auto &res = parser.get();
      out.status = static_cast<int>(res.result_int());
      for (const auto &field : res.base()) {
        out.headers.emplace(lowercase(field.name_string()),
                            toString(field.value()));
      }
      if (!res.keep_alive()) {
        closeUnlocked();
      }
    } else {
      auto res = bhttp::response<bhttp::string_body>{};
      stream_.expires_after(timeout);
      auto read_future =
          bhttp::async_read(stream_, buffer_, res, asio::use_future);
      waitForOperation(ioc_, read_future, timeout,
                       [this] { stream_.cancel(); });
      [[maybe_unused]] const auto bytes_read = read_future.get();
      out.status = static_cast<int>(res.result_int());
      out.body = res.body();
      for (const auto &field : res.base()) {
        out.headers.emplace(lowercase(field.name_string()),
                            toString(field.value()));
      }
      if (!res.keep_alive()) {
        closeUnlocked();
      }
    }

    return out;
  }

  std::string host_;
  std::string port_;
  asio::io_context ioc_;
  tcp::resolver resolver_;
  beast::tcp_stream stream_;
  beast::flat_buffer buffer_;
  bool connected_ = false;
  std::mutex mutex_;
};

auto connection_cache_mutex = std::mutex{};
auto connection_cache =
    std::map<std::string, std::shared_ptr<PooledConnection>>{};

http::ClientResponse
pooledRequest(std::string_view method, std::string_view url,
              std::string_view body = {},
              std::chrono::milliseconds timeout = std::chrono::seconds{30}) {
  const auto parsed = parseHttpUrl(url);
  auto connection = std::shared_ptr<PooledConnection>{};
  {
    auto lock = std::scoped_lock{connection_cache_mutex};
    auto &cached = connection_cache[connectionKey(parsed)];
    if (!cached) {
      cached = std::make_shared<PooledConnection>(parsed.host, parsed.port);
    }
    connection = cached;
  }
  return connection->request(method, parsed.target, body, timeout);
}

} // namespace

Response remoteGet(std::string_view url) {
  const auto res = pooledRequest("GET", url);
  requireStatus("remote_get", res.status, {200});
  return {.status = res.status, .body = res.body};
}

HeadResult remoteHeadInfo(std::string_view url,
                          std::chrono::milliseconds timeout) {
  const auto res = pooledRequest("HEAD", url, {}, timeout);
  return {
      .found = res.status == 200,
      .content_length = res.headerValue("Content-Length"),
      .etag = res.headerValue("ETag"),
      .last_modified = res.headerValue("Last-Modified"),
  };
}

bool remoteHead(std::string_view url, std::chrono::milliseconds timeout) {
  return remoteHeadInfo(url, timeout).found;
}

void remotePut(std::string_view url, std::string_view body) {
  const auto res = pooledRequest("PUT", url, body);
  requireStatus("remote_put", res.status, {201, 204});
}

void remotePutFiles(std::string_view url,
                    const std::vector<std::filesystem::path> &paths,
                    std::uint64_t content_length) {
  const auto parsed = parseHttpUrl(url);
  auto ioc = asio::io_context{};
  auto resolver = tcp::resolver{ioc};
  auto socket = tcp::socket{ioc};
  asio::connect(socket, resolver.resolve(parsed.host, parsed.port));

  auto req =
      bhttp::request<bhttp::empty_body>{bhttp::verb::put, parsed.target, 11};
  req.set(bhttp::field::host, parsed.host);
  req.set(bhttp::field::user_agent, "minikv");
  req.set(bhttp::field::content_type, "application/octet-stream");
  req.content_length(content_length);
  req.keep_alive(false);

  auto serializer = bhttp::request_serializer<bhttp::empty_body>{req};
  bhttp::write_header(socket, serializer);

  auto buffer = std::array<char, std::size_t{64} * 1024>{};
  for (const auto &path : paths) {
    auto file = std::ifstream{path, std::ios::binary};
    if (!file) {
      throw std::runtime_error("remote_put_files: failed to open part");
    }

    while (file) {
      file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const auto count = file.gcount();
      if (count > 0) {
        asio::write(socket, asio::buffer(buffer.data(),
                                         static_cast<std::size_t>(count)));
      }
    }
  }

  auto response_buffer = beast::flat_buffer{};
  auto response = bhttp::response<bhttp::string_body>{};
  bhttp::read(socket, response_buffer, response);

  auto ec = boost::system::error_code{};
  (void)socket.shutdown(tcp::socket::shutdown_both, ec);
  requireStatus("remote_put_files", static_cast<int>(response.result_int()),
                {201, 204});
}

void remoteDelete(std::string_view url) {
  const auto res = pooledRequest("DELETE", url);
  requireStatus("remote_delete", res.status, {204, 404});
}

void clearConnectionCache() {
  auto connections = std::vector<std::shared_ptr<PooledConnection>>{};
  {
    auto lock = std::scoped_lock{connection_cache_mutex};
    for (auto &[_, connection] : connection_cache) {
      if (connection) {
        connections.push_back(connection);
      }
    }
    connection_cache.clear();
  }

  for (auto &connection : connections) {
    if (connection) {
      connection->close();
    }
  }
}

} // namespace minikv::volume_client
