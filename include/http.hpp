#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace minikv::http {

struct Request {
  std::string method;
  std::string target;
  std::string path;
  std::unordered_map<std::string, std::string> params;
  std::map<std::string, std::string> headers;
  std::string body;

  [[nodiscard]] bool hasParam(std::string_view key) const;
  [[nodiscard]] std::string getParamValue(std::string_view key) const;
};

struct Response {
  int status = 200;
  std::map<std::string, std::string> headers;
  std::string body;

  void setHeader(std::string key, std::string value);
  void setContent(std::string content, std::string content_type);
  void set_header(std::string key, std::string value);
  void set_content(std::string content, std::string content_type);
};

using Handler = std::function<Response(const Request &)>;

class Server {
public:
  Server();
  explicit Server(Handler handler);
  ~Server();

  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;
  Server(Server &&) noexcept;
  Server &operator=(Server &&) noexcept;

  void setHandler(Handler handler);
  void setBodyLimit(std::uint64_t bytes);
  void setWorkerCount(std::size_t count);
  [[nodiscard]] int bindToAnyPort(std::string_view address);
  [[nodiscard]] bool listen(std::string_view address, int port);
  bool listenAfterBind();
  void waitUntilReady() const;
  void stop();
  [[nodiscard]] int port() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

struct ClientResponse {
  int status = 0;
  std::map<std::string, std::string> headers;
  std::string body;

  [[nodiscard]] std::string headerValue(std::string_view key) const;
  [[nodiscard]] std::string get_header_value(std::string_view key) const;
};

[[nodiscard]] ClientResponse
request(std::string_view method, std::string_view url,
        std::string_view body = {},
        std::string_view content_type = "application/octet-stream",
        std::chrono::milliseconds timeout = std::chrono::seconds{30});

} // namespace minikv::http
