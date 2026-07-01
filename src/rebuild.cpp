#include "rebuild.hpp"

#include "base64.hpp"
#include "placement.hpp"
#include "volume_client.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace minikv::rebuild {
namespace {

std::string toString(std::string_view value) {
  return std::string{value.data(), value.size()};
}

bool isHexByteName(std::string_view name) {
  if (name.size() != 2) {
    return false;
  }

  return std::ranges::all_of(name, [](const char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
  });
}

std::vector<FileEntry> getFiles(std::string_view url) {
  try {
    return parseDirectoryListing(volume_client::remoteGet(url).body);
  } catch (const std::exception &err) {
    std::cerr << "rebuild get_files error " << err.what() << " " << url
              << '\n';
    return {};
  }
}

bool parseVolume(index::LevelDbIndex &index, const Options &options,
                 std::string_view volume) {
  auto ok = true;
  const auto volume_string = toString(volume);
  for (const auto &first : getFiles("http://" + volume_string + "/")) {
    if (!isHashDirectory(first)) {
      continue;
    }

    for (const auto &second :
         getFiles("http://" + volume_string + "/" + first.name + "/")) {
      if (!isHashDirectory(second)) {
        continue;
      }

      const auto object_url =
          "http://" + volume_string + "/" + first.name + "/" + second.name +
          "/";
      for (const auto &object : getFiles(object_url)) {
        if (!rebuildObject(index, options, volume_string, object.name)) {
          ok = false;
        }
      }
    }
  }

  return ok;
}

} // namespace

bool isHashDirectory(const FileEntry &entry) {
  return entry.type == "directory" && isHexByteName(entry.name);
}

bool isSubvolumeDirectory(const FileEntry &entry) {
  return entry.type == "directory" && entry.name.size() == 4 &&
         entry.name.starts_with("sv");
}

std::vector<std::string>
orderedRebuiltVolumes(const std::vector<std::string> &recorded_volumes,
                      const std::vector<std::string> &preferred_volumes) {
  auto ordered = std::vector<std::string>{};
  ordered.reserve(recorded_volumes.size());

  for (const auto &preferred : preferred_volumes) {
    for (const auto &recorded : recorded_volumes) {
      if (preferred == recorded) {
        ordered.push_back(recorded);
      }
    }
  }

  for (const auto &recorded : recorded_volumes) {
    if (std::ranges::find(preferred_volumes, recorded) ==
        preferred_volumes.end()) {
      ordered.push_back(recorded);
    }
  }

  return ordered;
}

bool rebuildObject(index::LevelDbIndex &index, const Options &options,
                   std::string_view volume, std::string_view encoded_name) {
  auto key = std::string{};
  try {
    key = base64_decode(encoded_name);
  } catch (const std::exception &err) {
    std::cerr << "base64 decode error " << err.what() << '\n';
    return false;
  }

  const auto preferred = placement::key2volume(
      key, options.volumes, options.replicas, options.subvolumes);

  auto rec = index.getRecord(key);
  if (rec.deleted == record::Deleted::HARD) {
    rec = record::Record{{toString(volume)}, record::Deleted::NO, ""};
  } else {
    rec.rvolumes.push_back(toString(volume));
    rec.deleted = record::Deleted::NO;
    rec.hash = "";
  }

  const auto ordered = orderedRebuiltVolumes(rec.rvolumes, preferred);
  return index.putRecord(key, record::Record{ordered, record::Deleted::NO, ""});
}

std::vector<FileEntry> parseDirectoryListing(std::string_view body) {
  const auto parsed = boost::json::parse(body);
  const auto &items = parsed.as_array();
  auto entries = std::vector<FileEntry>{};
  entries.reserve(items.size());

  for (const auto &item : items) {
    const auto &object = item.as_object();
    const auto stringValue = [&object](boost::json::string_view key) {
      const auto *value = object.if_contains(key);
      if (value == nullptr || !value->is_string()) {
        return std::string{};
      }
      const auto string = value->as_string();
      return std::string{string.data(), string.size()};
    };
    entries.push_back(FileEntry{
        .name = stringValue("name"),
        .type = stringValue("type"),
        .mtime = stringValue("mtime"),
    });
  }

  return entries;
}

int run(const Options &options) {
  std::cout << "rebuilding on";
  for (const auto &volume : options.volumes) {
    std::cout << ' ' << volume;
  }
  std::cout << '\n';

  auto index = index::LevelDbIndex{options.db_path};
  if (!index.clear()) {
    return 1;
  }

  auto ok = true;
  for (const auto &volume : options.volumes) {
    auto has_subvolumes = false;
    for (const auto &entry : getFiles("http://" + volume + "/")) {
      if (!isSubvolumeDirectory(entry)) {
        continue;
      }

      has_subvolumes = true;
      if (!parseVolume(index, options, volume + "/" + entry.name)) {
        ok = false;
      }
    }

    if (!has_subvolumes && !parseVolume(index, options, volume)) {
      ok = false;
    }
  }

  return ok ? 0 : 1;
}

} // namespace minikv::rebuild
