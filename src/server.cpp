#include "server.hpp"

#include "hash.hpp"
#include "placement.hpp"
#include "record.hpp"
#include "volume_client.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <charconv>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace minikv::server {

namespace {

std::string toString(std::string_view value) {
  return std::string{value.data(), value.size()};
}

class KeyLockGuard {
public:
  KeyLockGuard(App &app, std::string_view key)
      : app_(app), key_(toString(key)), locked_(app_.lockKey(key_)) {}

  KeyLockGuard(const KeyLockGuard &) = delete;
  KeyLockGuard &operator=(const KeyLockGuard &) = delete;

  ~KeyLockGuard() {
    if (locked_) {
      app_.unlockKey(key_);
    }
  }

  bool locked() const { return locked_; }

private:
  App &app_;
  std::string key_;
  bool locked_;
};

std::string joinVolumes(const std::vector<std::string> &volumes) {
  auto joined = std::string{};
  for (const auto &volume : volumes) {
    if (!joined.empty()) {
      joined += ",";
    }
    joined += volume;
  }
  return joined;
}

void applyReadResult(const ReadResult &result, httplib::Response &res) {
  // Route handlers stay thin: App methods decide status and metadata, while
  // this adapter only translates the result into HTTP headers.
  res.status = result.status;
  res.set_header("Content-Length", "0");
  if (!result.content_md5.empty()) {
    res.set_header("Content-Md5", result.content_md5);
  }
  if (!result.key_volumes.empty()) {
    res.set_header("Key-Volumes", result.key_volumes);
  }
  if (!result.key_balance.empty()) {
    res.set_header("Key-Balance", result.key_balance);
  }
  if (!result.redirect_url.empty()) {
    res.set_header("Location", result.redirect_url);
  }
}

std::string firstQueryOperation(const httplib::Request &req) {
  const auto question = req.target.find('?');
  if (question == std::string::npos || question + 1 >= req.target.size()) {
    return "";
  }

  auto query = std::string_view{req.target}.substr(question + 1);
  const auto ampersand = query.find('&');
  if (ampersand != std::string_view::npos) {
    query = query.substr(0, ampersand);
  }

  const auto equals = query.find('=');
  if (equals != std::string_view::npos) {
    query = query.substr(0, equals);
  }

  return toString(query);
}

bool parseSize(std::string_view value, std::size_t &out) {
  const auto *begin = value.data();
  const auto *end = value.data() + value.size();
  auto parsed = std::size_t{0};
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }

  out = parsed;
  return true;
}

} // namespace

App::App(AppOptions options)
    : options_(std::move(options)), index_(options_.db_path) {}

bool App::lockKey(std::string_view key) {
  auto lock = std::scoped_lock{lock_mutex_};
  return locks_.insert(toString(key)).second;
}

void App::unlockKey(std::string_view key) {
  auto lock = std::scoped_lock{lock_mutex_};
  locks_.erase(toString(key));
}

record::Record App::getRecord(std::string_view key) const {
  return index_.getRecord(key);
}

bool App::putRecord(std::string_view key, const record::Record &rec) {
  return index_.putRecord(key, rec);
}

bool App::deleteRecord(std::string_view key) {
  return index_.deleteRecord(key);
}

WriteResult App::writeToReplicas(std::string_view key, std::string_view value) {
  // Match the Go write flow: mark metadata SOFT before touching volumes so
  // readers never see a partially replicated object as live.
  auto kvolumes = placement::key2volume(key, options_.volumes, options_.replicas,
                                        options_.subvolumes);

  auto pending = record::Record{kvolumes, record::Deleted::SOFT, ""};
  if (!putRecord(key, pending)) {
    return {500, pending};
  }

  auto keypath = placement::key2path(key);
  for (const auto &volume : kvolumes) {
    // Values are already materialized as a string_view here, unlike Go's
    // one-shot io.Reader, so each replica can reuse the same buffer.
    const auto remote = "http://" + volume + keypath;
    try {
      volume_client::remotePut(remote, value);
    } catch (const std::exception &) {
      return {500, pending};
    }
  }

  auto committed = record::Record{
      .rvolumes = kvolumes,
      .deleted = record::Deleted::NO,
      .hash = options_.md5sum ? md5_hex(value) : "",
  };
  if (!putRecord(key, committed)) {
    return {500, committed};
  }

  return {201, committed};
}

ReadResult App::readFromReplica(std::string_view key) {
  const auto rec = getRecord(key);
  auto result = ReadResult{
      .status = 404,
      .redirect_url = "",
      .record = rec,
      .content_md5 = rec.hash,
      .key_volumes = "",
      .key_balance = "",
  };

  if (rec.deleted == record::Deleted::SOFT ||
      rec.deleted == record::Deleted::HARD) {
    // Go falls back to a configured fallback server for missing/deleted keys;
    // otherwise the master returns a zero-length 404.
    if (options_.fallback.empty()) {
      return result;
    }

    result.status = 302;
    result.redirect_url = "http://" + options_.fallback + toString(key);
    return result;
  }

  if (rec.rvolumes.empty() || rec.rvolumes.front().empty()) {
    return result;
  }

  const auto kvolumes = placement::key2volume(key, options_.volumes,
                                              options_.replicas,
                                              options_.subvolumes);
  result.key_volumes = joinVolumes(rec.rvolumes);
  result.key_balance =
      placement::needs_rebalance(rec.rvolumes, kvolumes) ? "unbalanced"
                                                         : "balanced";

  const auto keypath = placement::key2path(key);
  for (const auto &volume : rec.rvolumes) {
    if (volume.empty()) {
      continue;
    }

    const auto remote = "http://" + volume + keypath;
    try {
      // Original minikeyvalue probes volume servers before redirecting. This
      // avoids sending clients to a replica that is already known missing/down.
      if (volume_client::remoteHead(remote, options_.volume_timeout)) {
        result.status = 302;
        result.redirect_url = remote;
        return result;
      }
    } catch (const std::exception &) {
    }
  }

  return result;
}

DeleteResult App::deleteFromReplicas(std::string_view key, bool unlink) {
  const auto rec = getRecord(key);
  if (rec.deleted == record::Deleted::HARD ||
      (unlink && rec.deleted == record::Deleted::SOFT)) {
    return {404, rec};
  }

  if (!unlink && options_.protect && rec.deleted == record::Deleted::NO) {
    return {403, rec};
  }

  auto pending = record::Record{
      .rvolumes = rec.rvolumes,
      .deleted = record::Deleted::SOFT,
      .hash = rec.hash,
  };
  if (!putRecord(key, pending)) {
    return {500, pending};
  }

  if (unlink) {
    // UNLINK is a virtual delete in the Go project: metadata becomes SOFT, but
    // remote volume files are left in place for later cleanup.
    return {204, pending};
  }

  const auto keypath = placement::key2path(key);
  auto delete_error = false;
  for (const auto &volume : rec.rvolumes) {
    const auto remote = "http://" + volume + keypath;
    try {
      volume_client::remoteDelete(remote);
    } catch (const std::exception &) {
      delete_error = true;
    }
  }

  if (delete_error) {
    return {500, pending};
  }

  if (!deleteRecord(key)) {
    return {500, pending};
  }

  return {204, record::Record{{}, record::Deleted::HARD, ""}};
}

QueryResult App::query(std::string_view key, std::string_view operation,
                       std::string_view start, std::string_view limit) const {
  if (operation != "list" && operation != "unlinked") {
    return {.status = 403, .content_type = "", .body = ""};
  }

  auto parsed_limit = std::size_t{0};
  if (!limit.empty() && !parseSize(limit, parsed_limit)) {
    return {.status = 400, .content_type = "", .body = ""};
  }

  auto keys = std::vector<std::string>{};
  auto next = std::string{};
  const auto records = index_.listRecords(key, start, 0);
  for (const auto &entry : records.records) {
    const auto include =
        (operation == "list" && entry.record.deleted == record::Deleted::NO) ||
        (operation == "unlinked" &&
         entry.record.deleted == record::Deleted::SOFT);
    if (!include) {
      continue;
    }

    if (keys.size() > 1000000) {
      return {.status = 413, .content_type = "", .body = ""};
    }

    if (parsed_limit > 0 && keys.size() == parsed_limit) {
      next = entry.key;
      break;
    }

    keys.push_back(entry.key);
  }

  const auto body = nlohmann::json{
      {"next", next},
      {"keys", keys},
  }.dump();

  return {.status = 200, .content_type = "application/json", .body = body};
}

const AppOptions &App::options() const { return options_; }

void registerRoutes(httplib::Server &server, App &app) {
  server.set_pre_routing_handler(
      [&app](const httplib::Request &req, httplib::Response &res) {
        // cpp-httplib has no Server::Head helper, so HEAD is handled before
        // normal routing and shares the GET/read decision path.
        if (req.method == "HEAD") {
          applyReadResult(app.readFromReplica(req.path), res);
          return httplib::Server::HandlerResponse::Handled;
        }

        return httplib::Server::HandlerResponse::Unhandled;
      });

  server.Put(R"(/.*)", [&app](const httplib::Request &req,
                              httplib::Response &res) {
    auto key_lock = KeyLockGuard{app, req.path};
    if (!key_lock.locked()) {
      res.status = 409;
      res.set_header("Content-Length", "0");
      return;
    }

    if (req.body.empty()) {
      res.status = 411;
      res.set_header("Content-Length", "0");
      return;
    }

    if (app.getRecord(req.path).deleted == record::Deleted::NO) {
      res.status = 403;
      res.set_header("Content-Length", "0");
      return;
    }

    const auto result = app.writeToReplicas(req.path, req.body);
    res.status = result.status;
    res.set_header("Content-Length", "0");
  });

  server.Get(R"(/.*)", [&app](const httplib::Request &req,
                              httplib::Response &res) {
    const auto operation = firstQueryOperation(req);
    if (!operation.empty()) {
      const auto result =
          app.query(req.path, operation, req.get_param_value("start"),
                    req.get_param_value("limit"));
      res.status = result.status;
      if (!result.content_type.empty()) {
        res.set_header("Content-Type", result.content_type);
      }
      if (!result.body.empty()) {
        res.set_content(result.body, result.content_type);
      } else {
        res.set_header("Content-Length", "0");
      }
      return;
    }

    applyReadResult(app.readFromReplica(req.path), res);
  });

  server.Delete(R"(/.*)", [&app](const httplib::Request &req,
                                 httplib::Response &res) {
    auto key_lock = KeyLockGuard{app, req.path};
    if (!key_lock.locked()) {
      res.status = 409;
      res.set_header("Content-Length", "0");
      return;
    }

    const auto result = app.deleteFromReplicas(req.path);
    res.status = result.status;
    res.set_header("Content-Length", "0");
  });
}

} // namespace minikv::server
