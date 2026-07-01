#include "server.hpp"

#include "hash.hpp"
#include "placement.hpp"
#include "rebalance.hpp"
#include "record.hpp"
#include "volume_client.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <charconv>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
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

http::Response makeEmptyResponse(int status) {
  auto res = http::Response{.status = status};
  res.setHeader("Content-Length", "0");
  return res;
}

http::Response applyReadResult(const ReadResult &result) {
  // Route handlers stay thin: App methods decide status and metadata, while
  // this adapter only translates the result into HTTP headers.
  auto res = http::Response{};
  res.status = result.status;
  res.setHeader("Content-Length", "0");
  if (!result.content_md5.empty()) {
    res.setHeader("Content-Md5", result.content_md5);
  }
  if (!result.key_volumes.empty()) {
    res.setHeader("Key-Volumes", result.key_volumes);
  }
  if (!result.key_balance.empty()) {
    res.setHeader("Key-Balance", result.key_balance);
  }
  if (!result.redirect_url.empty()) {
    res.setHeader("Location", result.redirect_url);
  }
  return res;
}

std::string firstQueryOperation(const http::Request &req) {
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

std::string xmlEscape(std::string_view value) {
  auto out = std::string{};
  out.reserve(value.size());
  for (const auto ch : value) {
    switch (ch) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    case '\'':
      out += "&apos;";
      break;
    default:
      out.push_back(ch);
      break;
    }
  }
  return out;
}

std::string xmlUnescape(std::string_view value) {
  auto out = toString(value);
  const auto replaceAll = [&out](std::string_view from, std::string_view to) {
    auto pos = std::size_t{0};
    while ((pos = out.find(from, pos)) != std::string::npos) {
      out.replace(pos, from.size(), to);
      pos += to.size();
    }
  };

  replaceAll("&lt;", "<");
  replaceAll("&gt;", ">");
  replaceAll("&quot;", "\"");
  replaceAll("&apos;", "'");
  replaceAll("&amp;", "&");
  return out;
}

std::vector<std::string> parseS3DeleteKeys(std::string_view body) {
  auto keys = std::vector<std::string>{};
  auto rest = body;
  static constexpr auto key_open = std::string_view{"<Key>"};
  static constexpr auto key_close = std::string_view{"</Key>"};

  while (true) {
    const auto open = rest.find(key_open);
    if (open == std::string_view::npos) {
      break;
    }
    rest.remove_prefix(open + key_open.size());
    const auto close = rest.find(key_close);
    if (close == std::string_view::npos) {
      throw std::runtime_error("malformed S3 delete XML");
    }
    keys.push_back(xmlUnescape(rest.substr(0, close)));
    rest.remove_prefix(close + key_close.size());
  }

  return keys;
}

std::vector<int> parseS3MultipartPartNumbers(std::string_view body) {
  auto parts = std::vector<int>{};
  auto rest = body;
  static constexpr auto part_open = std::string_view{"<PartNumber>"};
  static constexpr auto part_close = std::string_view{"</PartNumber>"};

  while (true) {
    const auto open = rest.find(part_open);
    if (open == std::string_view::npos) {
      break;
    }
    rest.remove_prefix(open + part_open.size());
    const auto close = rest.find(part_close);
    if (close == std::string_view::npos) {
      throw std::runtime_error("malformed S3 multipart XML");
    }

    auto part = 0;
    const auto value = rest.substr(0, close);
    const auto result =
        std::from_chars(value.data(), value.data() + value.size(), part);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        part <= 0) {
      throw std::runtime_error("invalid S3 multipart part number");
    }

    parts.push_back(part);
    rest.remove_prefix(close + part_close.size());
  }

  return parts;
}

std::string generateUploadId() {
  auto random = std::random_device{};
  auto out = std::ostringstream{};
  out << std::hex << std::setfill('0');
  for (auto i = 0; i < 4; ++i) {
    out << std::setw(8) << random();
  }
  return out.str();
}

} // namespace

App::App(AppOptions options)
    : options_(std::move(options)), index_(options_.db_path) {
  std::filesystem::remove_all(multipartRoot());
  std::filesystem::create_directories(multipartRoot());
}

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
  for (const auto index : replicaProbeOrder(rec.rvolumes.size())) {
    const auto &volume = rec.rvolumes[index];
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

RebalanceResult App::rebalanceReplicas(std::string_view key) {
  const auto rec = getRecord(key);
  if (rec.deleted != record::Deleted::NO) {
    return {.status = 404};
  }

  const auto target_volumes = placement::key2volume(
      key, options_.volumes, options_.replicas, options_.subvolumes);
  if (!rebalance::rebalanceObjectToTargets(index_, key, rec.rvolumes,
                                           target_volumes)) {
    return {.status = 400};
  }

  return {.status = 204};
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

  auto json_keys = boost::json::array{};
  json_keys.reserve(keys.size());
  for (const auto &key_item : keys) {
    json_keys.emplace_back(key_item);
  }

  auto json_body = boost::json::object{};
  json_body["next"] = next;
  json_body["keys"] = std::move(json_keys);
  const auto body = boost::json::serialize(json_body);

  return {.status = 200, .content_type = "application/json", .body = body};
}

QueryResult App::s3List(std::string_view bucket, std::string_view prefix) const {
  auto scan_prefix = toString(bucket);
  scan_prefix += "/";
  scan_prefix += prefix;

  auto body = std::ostringstream{};
  body << "<ListBucketResult>";
  const auto records = index_.listRecords(scan_prefix, "", 0);
  for (const auto &entry : records.records) {
    if (entry.record.deleted != record::Deleted::NO) {
      continue;
    }
    body << "<Contents><Key>"
         << xmlEscape(std::string_view{entry.key}.substr(scan_prefix.size()))
         << "</Key></Contents>";
  }
  body << "</ListBucketResult>";

  return {.status = 200, .content_type = "application/xml", .body = body.str()};
}

S3DeleteResult App::s3Delete(std::string_view bucket,
                             const std::vector<std::string> &keys) {
  for (const auto &key : keys) {
    auto full_key = toString(bucket);
    full_key += "/";
    full_key += key;
    const auto result = deleteFromReplicas(full_key);
    if (result.status != 204) {
      return {.status = result.status};
    }
  }
  return {.status = 204};
}

MultipartUploadResult App::createMultipartUpload(std::string_view key) {
  if (getRecord(key).deleted == record::Deleted::NO) {
    return {.status = 403, .upload_id = "", .body = ""};
  }

  auto upload_id = std::string{};
  {
    auto lock = std::scoped_lock{multipart_mutex_};
    do {
      upload_id = generateUploadId();
    } while (!upload_ids_.insert(upload_id).second);
  }

  const auto body = std::string{"<InitiateMultipartUploadResult>\n"
                                "        <UploadId>"} +
                    xmlEscape(upload_id) +
                    "</UploadId>\n"
                    "      </InitiateMultipartUploadResult>";
  return {.status = 200, .upload_id = upload_id, .body = body};
}

int App::writeMultipartPart(std::string_view upload_id, int part_number,
                            std::string_view body) {
  if (part_number <= 0) {
    return 403;
  }

  {
    auto lock = std::scoped_lock{multipart_mutex_};
    if (!upload_ids_.contains(toString(upload_id))) {
      return 403;
    }
  }

  std::filesystem::create_directories(multipartRoot());
  auto file = std::ofstream{multipartPartPath(upload_id, part_number),
                            std::ios::binary | std::ios::trunc};
  if (!file) {
    return 403;
  }
  file.write(body.data(), static_cast<std::streamsize>(body.size()));
  return file ? 200 : 403;
}

MultipartUploadResult App::completeMultipartUpload(
    std::string_view key, std::string_view upload_id,
    const std::vector<int> &part_numbers) {
  {
    auto lock = std::scoped_lock{multipart_mutex_};
    if (!upload_ids_.contains(toString(upload_id))) {
      return {.status = 403, .upload_id = "", .body = ""};
    }
  }

  for (const auto part_number : part_numbers) {
    if (!std::filesystem::exists(multipartPartPath(upload_id, part_number))) {
      return {.status = 403, .upload_id = "", .body = ""};
    }
  }

  auto body = std::string{};
  for (const auto part_number : part_numbers) {
    const auto path = multipartPartPath(upload_id, part_number);
    auto file = std::ifstream{path, std::ios::binary};
    if (!file) {
      return {.status = 403, .upload_id = "", .body = ""};
    }
    body.append(std::istreambuf_iterator<char>{file},
                std::istreambuf_iterator<char>{});
  }

  const auto result = writeToReplicas(key, body);
  if (result.status != 201) {
    return {.status = result.status, .upload_id = "", .body = ""};
  }

  for (const auto part_number : part_numbers) {
    std::filesystem::remove(multipartPartPath(upload_id, part_number));
  }

  {
    auto lock = std::scoped_lock{multipart_mutex_};
    upload_ids_.erase(toString(upload_id));
  }

  return {.status = result.status,
          .upload_id = "",
          .body = "<CompleteMultipartUploadResult></CompleteMultipartUploadResult>"};
}

std::filesystem::path App::multipartRoot() const {
  auto path = options_.db_path;
  path += ".multipart";
  return path;
}

std::filesystem::path App::multipartPartPath(std::string_view upload_id,
                                             int part_number) const {
  return multipartRoot() /
         (toString(upload_id) + "-" + std::to_string(part_number));
}

const AppOptions &App::options() const { return options_; }

std::vector<std::size_t> replicaProbeOrder(std::size_t count) {
  thread_local auto rng = [] {
    auto seed = std::random_device{}();
    return std::mt19937{seed};
  }();
  return replicaProbeOrder(count, rng);
}

std::vector<std::size_t> replicaProbeOrder(std::size_t count,
                                           std::mt19937 &rng) {
  auto order = std::vector<std::size_t>(count);
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::shuffle(order.begin(), order.end(), rng);
  return order;
}

http::Response handleRequest(App &app, const http::Request &req) {
  if (req.method == "HEAD") {
    return applyReadResult(app.readFromReplica(req.path));
  }

  if (req.method == "GET") {
    if (req.getParamValue("list-type") == "2") {
      const auto result = app.s3List(req.path, req.getParamValue("prefix"));
      auto res = http::Response{.status = result.status};
      res.setContent(result.body, result.content_type);
      return res;
    }

    const auto operation = firstQueryOperation(req);
    if (!operation.empty()) {
      const auto result =
          app.query(req.path, operation, req.getParamValue("start"),
                    req.getParamValue("limit"));
      auto res = http::Response{.status = result.status};
      res.status = result.status;
      if (!result.content_type.empty()) {
        res.setHeader("Content-Type", result.content_type);
      }
      if (!result.body.empty()) {
        res.setContent(result.body, result.content_type);
      } else {
        res.setHeader("Content-Length", "0");
      }
      return res;
    }

    return applyReadResult(app.readFromReplica(req.path));
  }

  if (req.method == "PUT") {
    auto lock_key = req.path + req.getParamValue("partNumber");
    auto key_lock = KeyLockGuard{app, lock_key};
    if (!key_lock.locked()) {
      return makeEmptyResponse(409);
    }

    if (req.body.empty()) {
      return makeEmptyResponse(411);
    }

    if (app.getRecord(req.path).deleted == record::Deleted::NO) {
      return makeEmptyResponse(403);
    }

    if (const auto part_number = req.getParamValue("partNumber");
        !part_number.empty()) {
      auto parsed_part_number = std::size_t{0};
      if (!parseSize(part_number, parsed_part_number) ||
          parsed_part_number == 0 ||
          parsed_part_number >
              static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return makeEmptyResponse(403);
      }

      return makeEmptyResponse(app.writeMultipartPart(
          req.getParamValue("uploadId"), static_cast<int>(parsed_part_number),
          req.body));
    }

    const auto result = app.writeToReplicas(req.path, req.body);
    return makeEmptyResponse(result.status);
  }

  if (req.method == "POST") {
    auto lock_key = req.path + req.getParamValue("partNumber");
    auto key_lock = KeyLockGuard{app, lock_key};
    if (!key_lock.locked()) {
      return makeEmptyResponse(409);
    }

    const auto operation = firstQueryOperation(req);
    if (operation == "uploads") {
      const auto result = app.createMultipartUpload(req.path);
      auto res = http::Response{.status = result.status};
      if (!result.body.empty()) {
        res.setContent(result.body, "application/xml");
      } else {
        res.setHeader("Content-Length", "0");
      }
      return res;
    }

    if (operation == "delete") {
      try {
        const auto keys = parseS3DeleteKeys(req.body);
        return makeEmptyResponse(app.s3Delete(req.path, keys).status);
      } catch (const std::exception &) {
        return makeEmptyResponse(500);
      }
    }

    if (!req.getParamValue("uploadId").empty()) {
      try {
        const auto parts = parseS3MultipartPartNumbers(req.body);
        const auto result =
            app.completeMultipartUpload(req.path, req.getParamValue("uploadId"),
                                        parts);
        auto res = http::Response{.status = result.status};
        if (!result.body.empty()) {
          res.setContent(result.body, "application/xml");
        } else {
          res.setHeader("Content-Length", "0");
        }
        return res;
      } catch (const std::exception &) {
        return makeEmptyResponse(500);
      }
    }

    return makeEmptyResponse(400);
  }

  if (req.method == "DELETE" || req.method == "UNLINK") {
    auto key_lock = KeyLockGuard{app, req.path};
    if (!key_lock.locked()) {
      return makeEmptyResponse(409);
    }

    const auto result = app.deleteFromReplicas(req.path, req.method == "UNLINK");
    return makeEmptyResponse(result.status);
  }

  if (req.method == "REBALANCE") {
    auto key_lock = KeyLockGuard{app, req.path};
    if (!key_lock.locked()) {
      return makeEmptyResponse(409);
    }

    return makeEmptyResponse(app.rebalanceReplicas(req.path).status);
  }

  return makeEmptyResponse(400);
}

void registerRoutes(http::Server &server, App &app) {
  server.setHandler([&app](const http::Request &req) {
    return handleRequest(app, req);
  });
}

} // namespace minikv::server
