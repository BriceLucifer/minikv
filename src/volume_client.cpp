#include "volume_client.hpp"

#include "http.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
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

} // namespace

Response remoteGet(std::string_view url) {
  const auto res = http::request("GET", url);
  requireStatus("remote_get", res.status, {200});
  return {.status = res.status, .body = res.body};
}

HeadResult remoteHeadInfo(std::string_view url,
                          std::chrono::milliseconds timeout) {
  const auto res =
      http::request("HEAD", url, {}, "application/octet-stream", timeout);
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
  const auto res = http::request("PUT", url, body);
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
  const auto res = http::request("DELETE", url);
  requireStatus("remote_delete", res.status, {204, 404});
}

} // namespace minikv::volume_client
