#pragma once

#include "index.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace minikv::rebalance {

struct Options {
  std::filesystem::path db_path;
  std::vector<std::string> volumes;
  int replicas = 3;
  int subvolumes = 10;
};

std::vector<std::string>
missingTargetVolumes(const std::vector<std::string> &real_volumes,
                     const std::vector<std::string> &target_volumes);
std::vector<std::string>
staleRealVolumes(const std::vector<std::string> &real_volumes,
                 const std::vector<std::string> &target_volumes);
bool rebalanceObject(index::LevelDbIndex &index, const Options &options,
                     std::string_view key,
                     const std::vector<std::string> &recorded_volumes);
int run(const Options &options);

} // namespace minikv::rebalance
