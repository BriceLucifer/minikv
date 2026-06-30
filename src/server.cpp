#include "server.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace minikv::server {

namespace {

std::string toString(std::string_view value) {
  return std::string{value.data(), value.size()};
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

const AppOptions &App::options() const { return options_; }

} // namespace minikv::server
