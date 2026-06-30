#include "server.hpp"

#include "hash.hpp"
#include "placement.hpp"
#include "record.hpp"
#include "volume_client.hpp"

#include <httplib.h>

#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace minikv::server {

namespace {

std::string toString(std::string_view value) {
  return std::string{value.data(), value.size()};
}

void applyReadResult(const ReadResult &result, httplib::Response &res) {
  res.status = result.status;
  res.set_header("Content-Length", "0");
  if (!result.redirect_url.empty()) {
    res.set_header("Location", result.redirect_url);
  }
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
  auto kvolumes = placement::key2volume(key, options_.volumes, options_.replicas,
                                        options_.subvolumes);

  auto pending = record::Record{kvolumes, record::Deleted::SOFT, ""};
  if (!putRecord(key, pending)) {
    return {500, pending};
  }

  auto keypath = placement::key2path(key);
  for (const auto &volume : kvolumes) {
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
  if (rec.deleted != record::Deleted::NO || rec.rvolumes.empty() ||
      rec.rvolumes.front().empty()) {
    return ReadResult{
        .status = 404,
        .redirect_url = "",
        .record = rec,
    };
  }

  const auto redirect_url = "http://" + rec.rvolumes.front() + placement::key2path(key);
  return ReadResult{
      .status = 307,
      .redirect_url = redirect_url,
      .record = rec,
  };
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

const AppOptions &App::options() const { return options_; }

void registerRoutes(httplib::Server &server, App &app) {
  server.set_pre_routing_handler(
      [&app](const httplib::Request &req, httplib::Response &res) {
        if (req.method == "HEAD") {
          applyReadResult(app.readFromReplica(req.path), res);
          return httplib::Server::HandlerResponse::Handled;
        }

        return httplib::Server::HandlerResponse::Unhandled;
      });

  server.Put(R"(/.*)", [&app](const httplib::Request &req,
                              httplib::Response &res) {
    const auto result = app.writeToReplicas(req.path, req.body);
    res.status = result.status;
    res.set_header("Content-Length", "0");
  });

  server.Get(R"(/.*)", [&app](const httplib::Request &req,
                              httplib::Response &res) {
    applyReadResult(app.readFromReplica(req.path), res);
  });

  server.Delete(R"(/.*)", [&app](const httplib::Request &req,
                                 httplib::Response &res) {
    const auto result = app.deleteFromReplicas(req.path);
    res.status = result.status;
    res.set_header("Content-Length", "0");
  });
}

} // namespace minikv::server
