#include "http.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace minikv::http {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = boost::beast::http;
using tcp = asio::ip::tcp;

namespace {

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

int hexValue(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

std::string percentDecode(std::string_view value) {
  auto out = std::string{};
  out.reserve(value.size());

  for (auto i = std::size_t{0}; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      const auto high = hexValue(value[i + 1]);
      const auto low = hexValue(value[i + 2]);
      if (high >= 0 && low >= 0) {
        out.push_back(static_cast<char>((high << 4) | low));
        i += 2;
        continue;
      }
    }
    if (value[i] == '+') {
      out.push_back(' ');
      continue;
    }
    out.push_back(value[i]);
  }

  return out;
}

std::unordered_map<std::string, std::string>
parseParams(std::string_view query) {
  auto params = std::unordered_map<std::string, std::string>{};
  while (!query.empty()) {
    const auto amp = query.find('&');
    const auto item = query.substr(0, amp);
    const auto eq = item.find('=');
    if (eq == std::string_view::npos) {
      params.emplace(percentDecode(item), "");
    } else {
      params.emplace(percentDecode(item.substr(0, eq)),
                     percentDecode(item.substr(eq + 1)));
    }

    if (amp == std::string_view::npos) {
      break;
    }
    query.remove_prefix(amp + 1);
  }
  return params;
}

void splitTarget(std::string_view target, std::string &path,
                 std::unordered_map<std::string, std::string> &params) {
  const auto question = target.find('?');
  if (question == std::string_view::npos) {
    path = percentDecode(target);
    params = {};
    return;
  }

  path = percentDecode(target.substr(0, question));
  params = parseParams(target.substr(question + 1));
}

struct ParsedUrl {
  std::string host;
  std::string port;
  std::string target;
};

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

  return {host, port, target};
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
      throw std::runtime_error("http request timed out");
    }
    ioc.run_for(deadline - now);
  }
}

Request fromBeastRequest(const bhttp::request<bhttp::string_body> &req) {
  auto out = Request{};
  out.method = toString(req.method_string());
  out.target = toString(req.target());
  splitTarget(out.target, out.path, out.params);
  out.body = req.body();
  for (const auto &field : req.base()) {
    out.headers.emplace(lowercase(field.name_string()),
                        toString(field.value()));
  }
  return out;
}

bhttp::response<bhttp::string_body> toBeastResponse(const Response &res,
                                                    bool head_response) {
  auto out = bhttp::response<bhttp::string_body>{
      static_cast<bhttp::status>(res.status), 11};
  for (const auto &[key, value] : res.headers) {
    out.set(key, value);
  }
  if (!head_response) {
    out.body() = res.body;
    out.prepare_payload();
  } else if (!out.has_content_length()) {
    out.content_length(res.body.size());
  }
  out.keep_alive(false);
  return out;
}

} // namespace

std::size_t workerCount() {
  const auto count = std::thread::hardware_concurrency();
  return std::max<std::size_t>(2, count == 0 ? 2 : count);
}

bool Request::hasParam(std::string_view key) const {
  return params.contains(toString(key));
}

std::string Request::getParamValue(std::string_view key) const {
  const auto it = params.find(toString(key));
  if (it == params.end()) {
    return "";
  }
  return it->second;
}

void Response::setHeader(std::string key, std::string value) {
  headers[std::move(key)] = std::move(value);
}

void Response::setContent(std::string content, std::string content_type) {
  body = std::move(content);
  setHeader("Content-Type", std::move(content_type));
}

void Response::set_header(std::string key, std::string value) {
  setHeader(std::move(key), std::move(value));
}

void Response::set_content(std::string content, std::string content_type) {
  setContent(std::move(content), std::move(content_type));
}

class Server::Impl {
public:
  Impl() : acceptor_(io_), workers_(workerCount()) {}

  void setHandler(Handler handler) {
    auto lock = std::scoped_lock{handler_mutex_};
    handler_ = std::move(handler);
  }

  void setBodyLimit(std::uint64_t bytes) { body_limit_ = bytes; }

  int bindToAnyPort(std::string_view address) {
    auto ec = boost::system::error_code{};
    auto endpoint =
        tcp::endpoint{asio::ip::make_address(toString(address), ec), 0};
    if (ec) {
      return -1;
    }

    (void)acceptor_.open(endpoint.protocol(), ec);
    if (ec) {
      return -1;
    }
    (void)acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
      return -1;
    }
    (void)acceptor_.bind(endpoint, ec);
    if (ec) {
      return -1;
    }
    (void)acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
      return -1;
    }

    port_ = acceptor_.local_endpoint().port();
    return port_;
  }

  bool listen(std::string_view address, int port) {
    auto ec = boost::system::error_code{};
    auto endpoint = tcp::endpoint{asio::ip::make_address(toString(address), ec),
                                  static_cast<unsigned short>(port)};
    if (ec) {
      return false;
    }

    (void)acceptor_.open(endpoint.protocol(), ec);
    if (ec) {
      return false;
    }
    (void)acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
      return false;
    }
    (void)acceptor_.bind(endpoint, ec);
    if (ec) {
      return false;
    }
    (void)acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
      return false;
    }

    port_ = acceptor_.local_endpoint().port();
    return listenAfterBind();
  }

  bool listenAfterBind() {
    ready_.store(true);
    while (!stopped_.load()) {
      auto socket = tcp::socket{io_};
      auto ec = boost::system::error_code{};
      (void)acceptor_.accept(socket, ec);
      if (ec) {
        if (stopped_.load()) {
          break;
        }
        continue;
      }
      if (stopped_.load()) {
        break;
      }

      asio::post(workers_, [this, socket = std::move(socket)]() mutable {
        handleSession(std::move(socket));
      });
    }
    return !stopped_.load();
  }

  void waitUntilReady() const {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!ready_.load()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        throw std::runtime_error("HTTP server did not become ready");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }

  void stop() {
    if (stopped_.exchange(true)) {
      return;
    }
    wakeAccept();
    auto ec = boost::system::error_code{};
    (void)acceptor_.close(ec);
    io_.stop();
    workers_.stop();
    workers_.join();
  }

  int port() const { return port_; }

private:
  Handler handler() const {
    auto lock = std::scoped_lock{handler_mutex_};
    return handler_;
  }

  void handleSession(tcp::socket socket) const {
    try {
      auto buffer = beast::flat_buffer{};
      auto parser = bhttp::request_parser<bhttp::string_body>{};
      parser.body_limit(body_limit_);
      bhttp::read(socket, buffer, parser);
      auto req = parser.release();

      auto h = handler();
      auto res = h ? h(fromBeastRequest(req))
                   : Response{.status = 404, .headers = {}, .body = ""};
      auto beast_res = toBeastResponse(res, req.method() == bhttp::verb::head);
      bhttp::write(socket, beast_res);

      auto ec = boost::system::error_code{};
      (void)socket.shutdown(tcp::socket::shutdown_send, ec);
    } catch (const boost::system::system_error &ex) {
      if (ex.code() == bhttp::error::body_limit) {
        auto res =
            bhttp::response<bhttp::string_body>{bhttp::status::payload_too_large,
                                                11};
        res.content_length(0);
        res.keep_alive(false);
        auto ec = boost::system::error_code{};
        (void)bhttp::write(socket, res, ec);
        (void)socket.shutdown(tcp::socket::shutdown_send, ec);
      }
    } catch (const std::exception &ex) {
      (void)ex;
    }
  }

  mutable std::mutex handler_mutex_;
  void wakeAccept() {
    if (port_ <= 0) {
      return;
    }

    try {
      auto wake_io = asio::io_context{};
      auto socket = tcp::socket{wake_io};
      auto ec = boost::system::error_code{};
      const auto endpoint =
          tcp::endpoint{asio::ip::make_address("127.0.0.1", ec),
                        static_cast<unsigned short>(port_)};
      if (ec) {
        return;
      }
      (void)socket.connect(endpoint, ec);
    } catch (const std::exception &ex) {
      (void)ex;
    }
  }

  Handler handler_;
  asio::io_context io_;
  tcp::acceptor acceptor_;
  asio::thread_pool workers_;
  std::atomic_bool ready_{false};
  std::atomic_bool stopped_{false};
  std::uint64_t body_limit_ = std::numeric_limits<std::uint64_t>::max();
  int port_ = -1;
};

Server::Server() : impl_(std::make_unique<Impl>()) {}

Server::Server(Handler handler) : Server() { setHandler(std::move(handler)); }

Server::~Server() {
  if (impl_) {
    impl_->stop();
  }
}

Server::Server(Server &&) noexcept = default;

Server &Server::operator=(Server &&) noexcept = default;

void Server::setHandler(Handler handler) {
  impl_->setHandler(std::move(handler));
}

void Server::setBodyLimit(std::uint64_t bytes) { impl_->setBodyLimit(bytes); }

int Server::bindToAnyPort(std::string_view address) {
  return impl_->bindToAnyPort(address);
}

bool Server::listen(std::string_view address, int port) {
  return impl_->listen(address, port);
}

bool Server::listenAfterBind() { return impl_->listenAfterBind(); }

void Server::waitUntilReady() const { impl_->waitUntilReady(); }

void Server::stop() { impl_->stop(); }

int Server::port() const { return impl_->port(); }

std::string ClientResponse::headerValue(std::string_view key) const {
  const auto it = headers.find(lowercase(key));
  if (it == headers.end()) {
    return "";
  }
  return it->second;
}

std::string ClientResponse::get_header_value(std::string_view key) const {
  return headerValue(key);
}

ClientResponse request(std::string_view method, std::string_view url,
                       std::string_view body, std::string_view content_type,
                       std::chrono::milliseconds timeout) {
  const auto parsed = parseHttpUrl(url);

  auto ioc = asio::io_context{};
  auto resolver = tcp::resolver{ioc};
  auto stream = beast::tcp_stream{ioc};

  auto resolve_future =
      resolver.async_resolve(parsed.host, parsed.port, asio::use_future);
  waitForOperation(ioc, resolve_future, timeout,
                   [&resolver] { resolver.cancel(); });
  const auto endpoints = resolve_future.get();

  stream.expires_after(timeout);
  auto connect_future = stream.async_connect(endpoints, asio::use_future);
  waitForOperation(ioc, connect_future, timeout,
                   [&stream] { stream.cancel(); });
  [[maybe_unused]] const auto connected_endpoint = connect_future.get();

  auto req = bhttp::request<bhttp::string_body>{};
  req.version(11);
  req.method_string(method);
  req.target(parsed.target);
  req.set(bhttp::field::host, parsed.host + ":" + parsed.port);
  req.set(bhttp::field::user_agent, "minikv-beast");
  if (!body.empty() || method == "PUT" || method == "POST") {
    req.set(bhttp::field::content_type, content_type);
    req.body() = toString(body);
  }
  req.prepare_payload();

  stream.expires_after(timeout);
  auto write_future = bhttp::async_write(stream, req, asio::use_future);
  waitForOperation(ioc, write_future, timeout, [&stream] { stream.cancel(); });
  [[maybe_unused]] const auto bytes_written = write_future.get();

  auto buffer = beast::flat_buffer{};
  auto out = ClientResponse{};
  if (lowercase(method) == "head") {
    auto parser = bhttp::response_parser<bhttp::empty_body>{};
    parser.skip(true);
    stream.expires_after(timeout);
    auto read_future =
        bhttp::async_read(stream, buffer, parser, asio::use_future);
    waitForOperation(ioc, read_future, timeout, [&stream] { stream.cancel(); });
    [[maybe_unused]] const auto bytes_read = read_future.get();
    const auto &res = parser.get();
    out.status = static_cast<int>(res.result_int());
    for (const auto &field : res.base()) {
      out.headers.emplace(lowercase(field.name_string()),
                          toString(field.value()));
    }
  } else {
    auto res = bhttp::response<bhttp::string_body>{};
    stream.expires_after(timeout);
    auto read_future = bhttp::async_read(stream, buffer, res, asio::use_future);
    waitForOperation(ioc, read_future, timeout, [&stream] { stream.cancel(); });
    [[maybe_unused]] const auto bytes_read = read_future.get();
    out.status = static_cast<int>(res.result_int());
    out.body = res.body();
    for (const auto &field : res.base()) {
      out.headers.emplace(lowercase(field.name_string()),
                          toString(field.value()));
    }
  }

  auto ec = boost::system::error_code{};
  (void)stream.socket().shutdown(tcp::socket::shutdown_both, ec);
  return out;
}

} // namespace minikv::http
