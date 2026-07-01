#include "cli.hpp"

#include <chrono>
#include <charconv>
#include <string>
#include <string_view>
#include <vector>

namespace minikv::cli {
namespace {

bool parseInt(std::string_view value, int &out) {
  const auto *begin = value.data();
  const auto *end = value.data() + value.size();
  auto parsed = 0;
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  out = parsed;
  return true;
}

bool parseBool(std::string_view value, bool &out) {
  if (value == "true" || value == "1") {
    out = true;
    return true;
  }
  if (value == "false" || value == "0") {
    out = false;
    return true;
  }
  return false;
}

std::vector<std::string> splitCommaSeparated(std::string_view value) {
  // Go's strings.Split would keep empty entries. For the CLI we reject empty
  // volume names by dropping them here, then validating count against replicas.
  auto out = std::vector<std::string>{};
  while (true) {
    const auto comma = value.find(',');
    const auto item = value.substr(0, comma);
    if (!item.empty()) {
      out.emplace_back(item);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    value.remove_prefix(comma + 1);
  }
  return out;
}

bool parseDuration(std::string_view value, std::chrono::milliseconds &out) {
  // Cover the duration forms used by the original flags in practice: plain
  // milliseconds, "ms", "s", "m", and "h".
  auto multiplier = 1;
  if (value.ends_with("ms")) {
    value.remove_suffix(2);
    multiplier = 1;
  } else if (value.ends_with('s')) {
    value.remove_suffix(1);
    multiplier = 1000;
  } else if (value.ends_with('m')) {
    value.remove_suffix(1);
    multiplier = 60 * 1000;
  } else if (value.ends_with('h')) {
    value.remove_suffix(1);
    multiplier = 60 * 60 * 1000;
  }

  auto amount = 0;
  if (!parseInt(value, amount)) {
    return false;
  }
  out = std::chrono::milliseconds{amount * multiplier};
  return true;
}

} // namespace

std::string usage() {
  return "Usage: ./mkv <server, rebuild, rebalance>\n"
         "  -db string\n"
         "        Path to leveldb\n"
         "  -fallback string\n"
         "        Fallback server for missing keys\n"
         "  -multipartttl duration\n"
         "        TTL for abandoned multipart uploads (default 24h)\n"
         "  -port int\n"
         "        Port for the server to listen on (default 3000)\n"
         "  -protect\n"
         "        Force UNLINK before DELETE\n"
         "  -replicas int\n"
         "        Amount of replicas to make of the data (default 3)\n"
         "  -subvolumes int\n"
         "        Amount of subvolumes, disks per machine (default 10)\n"
         "  -volumes string\n"
         "        Volumes to use for storage, comma separated\n";
}

ParseResult parseCommandLine(const std::vector<std::string> &args) {
  auto result = ParseResult{};
  // Defaults mirror the Go flags in src/main.go.
  result.options.app.replicas = 3;
  result.options.app.subvolumes = 10;
  result.options.app.md5sum = true;
  result.options.app.volume_timeout = std::chrono::seconds{1};

  for (auto i = std::size_t{0}; i < args.size(); ++i) {
    const auto arg = std::string_view{args[i]};
    auto nextValue = [&](std::string_view flag) -> std::string_view {
      if (i + 1 >= args.size()) {
        result.error = "missing value for " + std::string{flag};
        return {};
      }
      ++i;
      return args[i];
    };

    if (arg == "-port") {
      const auto value = nextValue(arg);
      if (!result.error.empty() || !parseInt(value, result.options.port)) {
        result.error = "invalid -port";
        return result;
      }
    } else if (arg == "-db") {
      result.options.app.db_path = std::string{nextValue(arg)};
    } else if (arg == "-fallback") {
      result.options.app.fallback = std::string{nextValue(arg)};
    } else if (arg == "-replicas") {
      const auto value = nextValue(arg);
      if (!result.error.empty() || !parseInt(value, result.options.app.replicas)) {
        result.error = "invalid -replicas";
        return result;
      }
    } else if (arg == "-subvolumes") {
      const auto value = nextValue(arg);
      if (!result.error.empty() || !parseInt(value, result.options.app.subvolumes)) {
        result.error = "invalid -subvolumes";
        return result;
      }
    } else if (arg == "-volumes") {
      result.options.app.volumes = splitCommaSeparated(nextValue(arg));
    } else if (arg == "-protect") {
      result.options.app.protect = true;
    } else if (arg.starts_with("-protect=")) {
      auto parsed = false;
      if (!parseBool(arg.substr(9), parsed)) {
        result.error = "invalid -protect";
        return result;
      }
      result.options.app.protect = parsed;
    } else if (arg == "-v") {
      result.options.verbose = true;
    } else if (arg == "-md5sum") {
      result.options.app.md5sum = true;
    } else if (arg.starts_with("-md5sum=")) {
      auto parsed = true;
      if (!parseBool(arg.substr(8), parsed)) {
        result.error = "invalid -md5sum";
        return result;
      }
      result.options.app.md5sum = parsed;
    } else if (arg == "-voltimeout") {
      const auto value = nextValue(arg);
      if (!result.error.empty() ||
          !parseDuration(value, result.options.app.volume_timeout)) {
        result.error = "invalid -voltimeout";
        return result;
      }
    } else if (arg == "-multipartttl") {
      const auto value = nextValue(arg);
      if (!result.error.empty() ||
          !parseDuration(value, result.options.app.multipart_upload_ttl)) {
        result.error = "invalid -multipartttl";
        return result;
      }
    } else if (arg.starts_with('-')) {
      result.error = "unknown flag " + std::string{arg};
      return result;
    } else if (result.options.command.empty()) {
      result.options.command = std::string{arg};
    } else {
      result.error = "unexpected argument " + std::string{arg};
      return result;
    }
  }

  if (result.options.command != "server" && result.options.command != "rebuild" &&
      result.options.command != "rebalance") {
    result.error = usage();
    return result;
  }

  if (result.options.app.db_path.empty()) {
    result.error = "Need a path to the database";
    return result;
  }

  if (static_cast<int>(result.options.app.volumes.size()) <
      result.options.app.replicas) {
    result.error = "Need at least as many volumes as replicas";
    return result;
  }

  result.ok = true;
  return result;
}

} // namespace minikv::cli
