#include "server.hpp"

#include "hash.hpp"
#include "placement.hpp"
#include "rebalance.hpp"
#include "record.hpp"
#include "volume_client.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
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

  ~KeyLockGuard() noexcept {
    if (locked_) {
      try {
        app_.unlockKey(key_);
      } catch (...) {
        std::terminate();
      }
    }
  }

  [[nodiscard]] bool locked() const { return locked_; }

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

std::string xmlEscape(std::string_view value);

http::Response makeEmptyResponse(int status) {
  auto res = http::Response{};
  res.status = status;
  res.setHeader("Content-Length", "0");
  return res;
}

http::Response makeS3ErrorResponse(int status, std::string_view code,
                                   std::string_view message,
                                   std::string_view resource) {
  auto body = std::ostringstream{};
  body << "<Error><Code>" << xmlEscape(code) << "</Code><Message>"
       << xmlEscape(message) << "</Message>";
  if (!resource.empty()) {
    body << "<Resource>" << xmlEscape(resource) << "</Resource>";
  }
  body << "</Error>";

  auto res = http::Response{};
  res.status = status;
  res.setContent(body.str(), "application/xml");
  return res;
}

std::string quotedEtag(std::string_view body) {
  return "\"" + md5_hex(body) + "\"";
}

std::string remoteUrl(std::string_view volume, std::string_view keypath) {
  auto remote = std::string{"http://"};
  remote += volume;
  remote += keypath;
  return remote;
}

template <typename Fn>
bool runReplicaOperationParallel(std::string_view operation,
                                 const std::vector<std::string> &volumes,
                                 std::string_view keypath, Fn &&fn) {
  auto futures = std::vector<std::pair<std::string, std::future<void>>>{};
  futures.reserve(volumes.size());
  for (const auto &volume : volumes) {
    auto remote = remoteUrl(volume, keypath);
    futures.emplace_back(
        remote, std::async(std::launch::async, [remote, &fn] { fn(remote); }));
  }

  auto ok = true;
  for (auto &[remote, future] : futures) {
    try {
      future.get();
    } catch (const std::exception &ex) {
      std::cerr << operation << " error " << ex.what() << " " << remote << '\n';
      ok = false;
    }
  }
  return ok;
}

template <typename Fn>
bool runReplicaOperationSerial(std::string_view operation,
                               const std::vector<std::string> &volumes,
                               std::string_view keypath, Fn &&fn) {
  auto ok = true;
  for (const auto &volume : volumes) {
    const auto remote = remoteUrl(volume, keypath);
    try {
      fn(remote);
    } catch (const std::exception &ex) {
      std::cerr << operation << " error " << ex.what() << " " << remote << '\n';
      ok = false;
    }
  }
  return ok;
}

bool containsCaseInsensitive(std::string_view haystack,
                             std::string_view needle) {
  const auto it =
      std::search(haystack.begin(), haystack.end(), needle.begin(),
                  needle.end(), [](char left, char right) {
                    return std::tolower(static_cast<unsigned char>(left)) ==
                           std::tolower(static_cast<unsigned char>(right));
                  });
  return it != haystack.end();
}

std::string requestHeader(const http::Request &req, std::string_view name) {
  const auto it = req.headers.find(toString(name));
  if (it == req.headers.end()) {
    return "";
  }
  return it->second;
}

bool isAwsChunkedPayload(const http::Request &req) {
  const auto content_encoding = requestHeader(req, "content-encoding");
  if (containsCaseInsensitive(content_encoding, "aws-chunked")) {
    return true;
  }

  const auto payload_hash = requestHeader(req, "x-amz-content-sha256");
  return containsCaseInsensitive(payload_hash, "STREAMING-");
}

std::string decodeAwsChunkedPayload(std::string_view body) {
  auto out = std::string{};
  auto offset = std::size_t{0};

  while (offset < body.size()) {
    const auto line_end = body.find("\r\n", offset);
    if (line_end == std::string_view::npos) {
      throw std::runtime_error("malformed aws-chunked payload");
    }

    const auto header = body.substr(offset, line_end - offset);
    const auto extension = header.find(';');
    const auto size_text = header.substr(0, extension);
    auto chunk_size = std::size_t{0};
    const auto *begin = size_text.data();
    const auto *end = begin + size_text.size();
    const auto parsed = std::from_chars(begin, end, chunk_size, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
      throw std::runtime_error("invalid aws-chunked size");
    }

    offset = line_end + 2;
    if (chunk_size == 0) {
      return out;
    }
    if (offset + chunk_size + 2 > body.size() ||
        body.substr(offset + chunk_size, 2) != "\r\n") {
      throw std::runtime_error("truncated aws-chunked payload");
    }

    out.append(body.substr(offset, chunk_size));
    offset += chunk_size + 2;
  }

  throw std::runtime_error("missing aws-chunked terminator");
}

std::string requestBodyForStorage(const http::Request &req) {
  if (!isAwsChunkedPayload(req)) {
    return req.body;
  }
  return decodeAwsChunkedPayload(req.body);
}

struct ObjectMetadata {
  std::string size;
  std::string etag;
  std::string last_modified;
};

ObjectMetadata objectMetadataFromReplicas(const record::Record &rec,
                                          std::string_view key,
                                          std::chrono::milliseconds timeout) {
  const auto keypath = placement::key2path(key);
  for (const auto &volume : rec.rvolumes) {
    if (volume.empty()) {
      continue;
    }

    try {
      const auto head =
          volume_client::remoteHeadInfo(remoteUrl(volume, keypath), timeout);
      if (head.found) {
        return {.size = head.content_length,
                .etag = head.etag,
                .last_modified = head.last_modified};
      }
    } catch (const std::exception &ex) {
      [[maybe_unused]] const auto *ignored_replica_error = &ex;
      // Try the next replica; unavailable replicas should not fail HEAD.
    }
  }

  return {};
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

http::Response applyHeadMetadata(App &app, std::string_view key) {
  const auto rec = app.getRecord(key);
  if (rec.deleted != record::Deleted::NO) {
    return applyReadResult(app.readFromReplica(key));
  }

  const auto metadata =
      objectMetadataFromReplicas(rec, key, app.options().volume_timeout);
  if (metadata.size.empty()) {
    return makeEmptyResponse(404);
  }

  auto res = http::Response{};
  res.status = 200;
  res.setHeader("Content-Length", metadata.size);
  res.setHeader("Accept-Ranges", "bytes");
  res.setHeader("x-amz-storage-class", "STANDARD");
  if (!metadata.etag.empty()) {
    res.setHeader("ETag", metadata.etag);
  }
  if (!metadata.last_modified.empty()) {
    res.setHeader("Last-Modified", metadata.last_modified);
  }
  if (!rec.hash.empty()) {
    res.setHeader("Content-Md5", rec.hash);
  }
  if (!rec.rvolumes.empty()) {
    res.setHeader("Key-Volumes", joinVolumes(rec.rvolumes));
  }

  const auto preferred =
      placement::key2volume(key, app.options().volumes, app.options().replicas,
                            app.options().subvolumes);
  res.setHeader("Key-Balance",
                placement::needs_rebalance(rec.rvolumes, preferred)
                    ? "unbalanced"
                    : "balanced");
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
  auto kvolumes = placement::key2volume(key, options_.volumes,
                                        options_.replicas, options_.subvolumes);

  auto pending = record::Record{kvolumes, record::Deleted::SOFT, ""};
  if (!putRecord(key, pending)) {
    return {500, pending};
  }

  const auto keypath = placement::key2path(key);
  const auto write_replica = [&](const std::string &remote) {
    // Values are already materialized as a string_view here, unlike Go's
    // one-shot io.Reader, so each replica can reuse the same buffer.
    volume_client::remotePut(remote, value);
  };
  const auto replicas_written =
      options_.parallel_replica_io
          ? runReplicaOperationParallel("replica put", kvolumes, keypath,
                                        write_replica)
          : runReplicaOperationSerial("replica put", kvolumes, keypath,
                                      write_replica);
  if (!replicas_written) {
    return {500, pending};
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

WriteResult
App::writeFilesToReplicas(std::string_view key,
                          const std::vector<std::filesystem::path> &paths,
                          std::uint64_t content_length, std::string_view hash) {
  auto kvolumes = placement::key2volume(key, options_.volumes,
                                        options_.replicas, options_.subvolumes);

  auto pending = record::Record{kvolumes, record::Deleted::SOFT, ""};
  if (!putRecord(key, pending)) {
    return {500, pending};
  }

  const auto keypath = placement::key2path(key);
  const auto write_replica_files = [&](const std::string &remote) {
    volume_client::remotePutFiles(remote, paths, content_length);
  };
  const auto replicas_written =
      options_.parallel_replica_io
          ? runReplicaOperationParallel("replica put files", kvolumes, keypath,
                                        write_replica_files)
          : runReplicaOperationSerial("replica put files", kvolumes, keypath,
                                      write_replica_files);
  if (!replicas_written) {
    return {500, pending};
  }

  auto committed = record::Record{
      .rvolumes = kvolumes,
      .deleted = record::Deleted::NO,
      .hash = options_.md5sum ? toString(hash) : "",
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
    result.redirect_url = remoteUrl(options_.fallback, key);
    return result;
  }

  if (rec.rvolumes.empty() || rec.rvolumes.front().empty()) {
    return result;
  }

  const auto kvolumes = placement::key2volume(
      key, options_.volumes, options_.replicas, options_.subvolumes);
  result.key_volumes = joinVolumes(rec.rvolumes);
  result.key_balance = placement::needs_rebalance(rec.rvolumes, kvolumes)
                           ? "unbalanced"
                           : "balanced";

  const auto keypath = placement::key2path(key);
  for (const auto index : replicaProbeOrder(rec.rvolumes.size())) {
    const auto &volume = rec.rvolumes[index];
    if (volume.empty()) {
      continue;
    }

    const auto remote = remoteUrl(volume, keypath);
    try {
      // Original minikeyvalue probes volume servers before redirecting. This
      // avoids sending clients to a replica that is already known missing/down.
      if (volume_client::remoteHead(remote, options_.volume_timeout)) {
        result.status = 302;
        result.redirect_url = remote;
        return result;
      }
    } catch (const std::exception &ex) {
      [[maybe_unused]] const auto *ignored_replica_error = &ex;
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
  const auto delete_replica = [](const std::string &remote) {
    volume_client::remoteDelete(remote);
  };
  const auto replicas_deleted =
      options_.parallel_replica_io
          ? runReplicaOperationParallel("replica delete", rec.rvolumes, keypath,
                                        delete_replica)
          : runReplicaOperationSerial("replica delete", rec.rvolumes, keypath,
                                      delete_replica);
  if (!replicas_deleted) {
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

QueryResult App::s3List(std::string_view bucket,
                        std::string_view prefix) const {
  auto scan_prefix = toString(bucket);
  scan_prefix += "/";
  scan_prefix += prefix;

  auto contents = std::ostringstream{};
  auto key_count = std::size_t{0};
  const auto records = index_.listRecords(scan_prefix, "", 0);
  for (const auto &entry : records.records) {
    if (entry.record.deleted != record::Deleted::NO) {
      continue;
    }
    const auto metadata = objectMetadataFromReplicas(entry.record, entry.key,
                                                     options_.volume_timeout);
    ++key_count;
    contents << "<Contents><Key>"
             << xmlEscape(
                    std::string_view{entry.key}.substr(scan_prefix.size()))
             << "</Key>";
    if (!metadata.last_modified.empty()) {
      contents << "<LastModified>" << xmlEscape(metadata.last_modified)
               << "</LastModified>";
    }
    if (!metadata.size.empty()) {
      contents << "<Size>" << xmlEscape(metadata.size) << "</Size>";
    }
    if (!metadata.etag.empty()) {
      contents << "<ETag>" << xmlEscape(metadata.etag) << "</ETag>";
    }
    contents << "<StorageClass>STANDARD</StorageClass></Contents>";
  }

  auto bucket_name = toString(bucket);
  if (!bucket_name.empty() && bucket_name.front() == '/') {
    bucket_name.erase(bucket_name.begin());
  }

  auto body = std::ostringstream{};
  body << "<ListBucketResult>"
       << "<Name>" << xmlEscape(bucket_name) << "</Name>"
       << "<Prefix>" << xmlEscape(prefix) << "</Prefix>"
       << "<KeyCount>" << key_count << "</KeyCount>"
       << "<MaxKeys>1000</MaxKeys>"
       << "<IsTruncated>false</IsTruncated>" << contents.str()
       << "</ListBucketResult>";

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
    return {.status = 403, .upload_id = "", .etag = "", .body = ""};
  }

  auto upload_id = std::string{};
  {
    auto lock = std::scoped_lock{multipart_mutex_};
    cleanupExpiredMultipartUploadsLocked(std::chrono::steady_clock::now());
    do {
      upload_id = generateUploadId();
    } while (upload_ids_.contains(upload_id));
    upload_ids_[upload_id] = std::chrono::steady_clock::now();
  }

  const auto body = std::string{"<InitiateMultipartUploadResult>\n"
                                "        <UploadId>"} +
                    xmlEscape(upload_id) +
                    "</UploadId>\n"
                    "      </InitiateMultipartUploadResult>";
  return {.status = 200, .upload_id = upload_id, .etag = "", .body = body};
}

int App::writeMultipartPart(std::string_view upload_id, int part_number,
                            std::string_view body) {
  if (part_number <= 0) {
    return 403;
  }

  auto lock = std::scoped_lock{multipart_mutex_};
  cleanupExpiredMultipartUploadsLocked(std::chrono::steady_clock::now());
  const auto upload = toString(upload_id);
  const auto found = upload_ids_.find(upload);
  if (found == upload_ids_.end()) {
    return 403;
  }

  std::filesystem::create_directories(multipartRoot());
  auto file = std::ofstream{multipartPartPath(upload_id, part_number),
                            std::ios::binary | std::ios::trunc};
  if (!file) {
    return 403;
  }
  file.write(body.data(), static_cast<std::streamsize>(body.size()));
  found->second = std::chrono::steady_clock::now();
  return file ? 200 : 403;
}

int App::abortMultipartUpload(std::string_view upload_id) {
  auto lock = std::scoped_lock{multipart_mutex_};
  cleanupExpiredMultipartUploadsLocked(std::chrono::steady_clock::now());
  const auto upload = toString(upload_id);
  if (upload_ids_.erase(upload) == 0) {
    return 404;
  }

  removeMultipartPartsLocked(upload);
  return 204;
}

MultipartUploadResult
App::completeMultipartUpload(std::string_view key, std::string_view upload_id,
                             const std::vector<int> &part_numbers) {
  if (getRecord(key).deleted == record::Deleted::NO) {
    return {.status = 403, .upload_id = "", .etag = "", .body = ""};
  }

  {
    auto lock = std::scoped_lock{multipart_mutex_};
    cleanupExpiredMultipartUploadsLocked(std::chrono::steady_clock::now());
    if (!upload_ids_.contains(toString(upload_id))) {
      return {.status = 403, .upload_id = "", .etag = "", .body = ""};
    }
  }

  for (const auto part_number : part_numbers) {
    if (!std::filesystem::exists(multipartPartPath(upload_id, part_number))) {
      return {.status = 403, .upload_id = "", .etag = "", .body = ""};
    }
  }

  auto part_paths = std::vector<std::filesystem::path>{};
  auto content_length = std::uint64_t{0};
  for (const auto part_number : part_numbers) {
    const auto path = multipartPartPath(upload_id, part_number);
    auto ec = std::error_code{};
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
      return {.status = 403, .upload_id = "", .etag = "", .body = ""};
    }
    part_paths.push_back(path);
    content_length += size;
  }

  const auto hash = md5_hex_files(part_paths);
  const auto etag = "\"" + hash + "\"";
  const auto result =
      writeFilesToReplicas(key, part_paths, content_length, hash);
  if (result.status != 201) {
    return {.status = result.status, .upload_id = "", .etag = "", .body = ""};
  }

  for (const auto part_number : part_numbers) {
    std::filesystem::remove(multipartPartPath(upload_id, part_number));
  }

  {
    auto lock = std::scoped_lock{multipart_mutex_};
    cleanupExpiredMultipartUploadsLocked(std::chrono::steady_clock::now());
    upload_ids_.erase(toString(upload_id));
  }

  return {.status = result.status,
          .upload_id = "",
          .etag = etag,
          .body = "<CompleteMultipartUploadResult><ETag>" + etag +
                  "</ETag></CompleteMultipartUploadResult>"};
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

void App::removeMultipartPartsLocked(std::string_view upload_id) {
  const auto root = multipartRoot();
  if (!std::filesystem::exists(root)) {
    return;
  }

  const auto prefix = toString(upload_id) + "-";
  for (const auto &entry : std::filesystem::directory_iterator{root}) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto name = entry.path().filename().string();
    if (name.starts_with(prefix)) {
      std::filesystem::remove(entry.path());
    }
  }
}

void App::cleanupExpiredMultipartUploadsLocked(
    std::chrono::steady_clock::time_point now) {
  if (options_.multipart_upload_ttl <= std::chrono::milliseconds{0}) {
    return;
  }

  for (auto it = upload_ids_.begin(); it != upload_ids_.end();) {
    if (now - it->second < options_.multipart_upload_ttl) {
      ++it;
      continue;
    }

    removeMultipartPartsLocked(it->first);
    it = upload_ids_.erase(it);
  }
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
    return applyHeadMetadata(app, req.path);
  }

  if (req.method == "GET") {
    if (req.getParamValue("list-type") == "2") {
      const auto result = app.s3List(req.path, req.getParamValue("prefix"));
      auto res = http::Response{};
      res.status = result.status;
      res.setContent(result.body, result.content_type);
      return res;
    }

    const auto operation = firstQueryOperation(req);
    if (!operation.empty()) {
      const auto result =
          app.query(req.path, operation, req.getParamValue("start"),
                    req.getParamValue("limit"));
      auto res = http::Response{};
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

    auto body = std::string{};
    try {
      body = requestBodyForStorage(req);
    } catch (const std::exception &) {
      return makeEmptyResponse(400);
    }

    if (body.empty()) {
      return makeEmptyResponse(411);
    }

    if (app.getRecord(req.path).deleted == record::Deleted::NO) {
      return makeS3ErrorResponse(403, "AccessDenied", "Object already exists",
                                 req.path);
    }

    if (const auto part_number = req.getParamValue("partNumber");
        !part_number.empty()) {
      auto parsed_part_number = std::size_t{0};
      if (!parseSize(part_number, parsed_part_number) ||
          parsed_part_number == 0 ||
          parsed_part_number >
              static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return makeS3ErrorResponse(400, "InvalidArgument",
                                   "Invalid multipart part number", req.path);
      }

      const auto part_status =
          app.writeMultipartPart(req.getParamValue("uploadId"),
                                 static_cast<int>(parsed_part_number), body);
      if (part_status == 403) {
        return makeS3ErrorResponse(404, "NoSuchUpload",
                                   "Multipart upload does not exist", req.path);
      }

      auto res = makeEmptyResponse(part_status);
      if (res.status == 200) {
        res.setHeader("ETag", quotedEtag(body));
      }
      return res;
    }

    const auto result = app.writeToReplicas(req.path, body);
    auto res = makeEmptyResponse(result.status);
    if (result.status == 201) {
      res.setHeader("ETag", quotedEtag(body));
    }
    return res;
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
      if (result.status == 403) {
        return makeS3ErrorResponse(403, "AccessDenied", "Object already exists",
                                   req.path);
      }

      auto res = http::Response{};
      res.status = result.status;
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
        return makeS3ErrorResponse(400, "MalformedXML",
                                   "The XML you provided was not well-formed",
                                   req.path);
      }
    }

    if (!req.getParamValue("uploadId").empty()) {
      try {
        const auto parts = parseS3MultipartPartNumbers(req.body);
        if (app.getRecord(req.path).deleted == record::Deleted::NO) {
          return makeS3ErrorResponse(403, "AccessDenied",
                                     "Object already exists", req.path);
        }

        const auto result = app.completeMultipartUpload(
            req.path, req.getParamValue("uploadId"), parts);
        if (result.status == 403) {
          return makeS3ErrorResponse(404, "NoSuchUpload",
                                     "Multipart upload does not exist or is "
                                     "missing a requested part",
                                     req.path);
        }

        auto res = http::Response{};
        res.status = result.status;
        if (!result.body.empty()) {
          res.setContent(result.body, "application/xml");
          if (!result.etag.empty()) {
            res.setHeader("ETag", result.etag);
          }
        } else {
          res.setHeader("Content-Length", "0");
        }
        return res;
      } catch (const std::exception &) {
        return makeS3ErrorResponse(400, "MalformedXML",
                                   "The XML you provided was not well-formed",
                                   req.path);
      }
    }

    return makeEmptyResponse(400);
  }

  if (req.method == "DELETE" || req.method == "UNLINK") {
    auto key_lock = KeyLockGuard{app, req.path};
    if (!key_lock.locked()) {
      return makeEmptyResponse(409);
    }

    if (req.method == "DELETE" && !req.getParamValue("uploadId").empty()) {
      const auto abort_status =
          app.abortMultipartUpload(req.getParamValue("uploadId"));
      if (abort_status == 404) {
        return makeS3ErrorResponse(404, "NoSuchUpload",
                                   "Multipart upload does not exist", req.path);
      }
      return makeEmptyResponse(abort_status);
    }

    const auto result =
        app.deleteFromReplicas(req.path, req.method == "UNLINK");
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
  server.setBodyLimit(app.options().max_body_size);
  server.setWorkerCount(app.options().http_workers);
  server.setHandler(
      [&app](const http::Request &req) { return handleRequest(app, req); });
}

} // namespace minikv::server
