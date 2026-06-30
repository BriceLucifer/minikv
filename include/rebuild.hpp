#pragma once

#include "index.hpp"
#include "record.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace minikv::rebuild {

struct Options {
  std::filesystem::path db_path;
  std::vector<std::string> volumes;
  int replicas = 3;
  int subvolumes = 10;
};

struct FileEntry {
  std::string name;
  std::string type;
  std::string mtime;
};

bool isHashDirectory(const FileEntry &entry);
bool isSubvolumeDirectory(const FileEntry &entry);
std::vector<std::string>
orderedRebuiltVolumes(const std::vector<std::string> &recorded_volumes,
                      const std::vector<std::string> &preferred_volumes);
bool rebuildObject(index::LevelDbIndex &index, const Options &options,
                   std::string_view volume, std::string_view encoded_name);
std::vector<FileEntry> parseDirectoryListing(std::string_view body);
int run(const Options &options);

} // namespace minikv::rebuild
