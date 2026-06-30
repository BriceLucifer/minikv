#include "placement.hpp"
#include "base64.hpp"
#include "hash.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace minikv::placement {

std::string key2path(std::string_view key) {
  auto mkey = md5(key);
  auto b64key = base64_encode(key);

  return std::format("/{:02x}/{:02x}/{}", static_cast<int>(mkey[0]),
                     static_cast<int>(mkey[1]), b64key);
}

std::vector<std::string> key2volume(std::string_view key,
                                    const std::vector<std::string> &volumes,
                                    int count, int svcount) {
  auto sortvols = byScore{};
  sortvols.reserve(volumes.size());

  for (const auto &volume : volumes) {
    std::string score_input;
    score_input.reserve(key.size() + volume.size());
    score_input.append(key);
    score_input.append(volume);

    sortvols.push_back(Sortvol{
        .score = md5(score_input),
        .volume = volume,
    });
  }

  std::stable_sort(sortvols.begin(), sortvols.end(),
                   [](const Sortvol &a, const Sortvol &b) {
                     return a.score > b.score;
                   });

  std::vector<std::string> ret;
  ret.reserve(static_cast<std::size_t>(count));

  for (int i = 0; i < count; i++) {
    const auto &sv = sortvols[static_cast<std::size_t>(i)];

    if (svcount == 1) {
      ret.push_back(sv.volume);
      continue;
    }

    const auto svhash =
        (static_cast<std::uint32_t>(sv.score[12]) << 24) |
        (static_cast<std::uint32_t>(sv.score[13]) << 16) |
        (static_cast<std::uint32_t>(sv.score[14]) << 8) |
        static_cast<std::uint32_t>(sv.score[15]);

    ret.push_back(std::format("{}/sv{:02X}", sv.volume, svhash % svcount));
  }

  return ret;
}

bool needs_rebalance(const std::vector<std::string> &volumes,
                     const std::vector<std::string> &kvolumes) {
  return volumes != kvolumes;
}

} // namespace minikv::placement
