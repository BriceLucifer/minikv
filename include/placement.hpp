#pragma once

#include "hash.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace minikv::placement {

// for sortvol
struct Sortvol {
  Md5Digest score;
  std::string volume;
};

// vector the sortval
using byScore = std::vector<Sortvol>;

std::string key2path(std::string_view key);
std::vector<std::string> key2volume(std::string_view key,
                                    const std::vector<std::string> &volumes,
                                    int count, int svcount);
bool needs_rebalance(const std::vector<std::string> &volumes,
                     const std::vector<std::string> &kvolumes);
} // namespace minikv::placement
