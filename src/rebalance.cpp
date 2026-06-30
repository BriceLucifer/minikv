#include "rebalance.hpp"

#include "placement.hpp"
#include "record.hpp"
#include "volume_client.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace minikv::rebalance {
namespace {

constexpr auto kProbeTimeout = std::chrono::minutes{1};

std::string toString(std::string_view value) {
  return std::string{value.data(), value.size()};
}

bool contains(const std::vector<std::string> &values,
              const std::string &needle) {
  return std::ranges::find(values, needle) != values.end();
}

std::vector<std::string>
existingRecordedVolumes(std::string_view key_path,
                        const std::vector<std::string> &recorded_volumes) {
  auto real_volumes = std::vector<std::string>{};
  for (const auto &volume : recorded_volumes) {
    const auto remote = "http://" + volume + toString(key_path);
    try {
      if (volume_client::remoteHead(remote, kProbeTimeout)) {
        real_volumes.push_back(volume);
      }
    } catch (const std::exception &err) {
      std::cerr << "rebalance head error " << err.what() << " " << remote
                << '\n';
      throw;
    }
  }

  return real_volumes;
}

bool readFromAnyRealVolume(std::string_view key_path,
                           const std::vector<std::string> &real_volumes,
                           std::string &body) {
  for (const auto &volume : real_volumes) {
    const auto remote = "http://" + volume + toString(key_path);
    try {
      body = volume_client::remoteGet(remote).body;
      return true;
    } catch (const std::exception &err) {
      std::cerr << "rebalance get error " << err.what() << " " << remote
                << '\n';
    }
  }

  return false;
}

} // namespace

std::vector<std::string>
missingTargetVolumes(const std::vector<std::string> &real_volumes,
                     const std::vector<std::string> &target_volumes) {
  auto missing = std::vector<std::string>{};
  for (const auto &target : target_volumes) {
    if (!contains(real_volumes, target)) {
      missing.push_back(target);
    }
  }
  return missing;
}

std::vector<std::string>
staleRealVolumes(const std::vector<std::string> &real_volumes,
                 const std::vector<std::string> &target_volumes) {
  auto stale = std::vector<std::string>{};
  for (const auto &real : real_volumes) {
    if (!contains(target_volumes, real)) {
      stale.push_back(real);
    }
  }
  return stale;
}

bool rebalanceObject(index::LevelDbIndex &index, const Options &options,
                     std::string_view key,
                     const std::vector<std::string> &recorded_volumes) {
  const auto key_path = placement::key2path(key);
  auto real_volumes = std::vector<std::string>{};
  try {
    real_volumes = existingRecordedVolumes(key_path, recorded_volumes);
  } catch (const std::exception &) {
    return false;
  }

  if (real_volumes.empty()) {
    std::cerr << "rebalance impossible, " << key << " is missing!\n";
    return false;
  }

  const auto target_volumes =
      placement::key2volume(key, options.volumes, options.replicas,
                            options.subvolumes);
  if (!placement::needs_rebalance(real_volumes, target_volumes)) {
    return true;
  }

  std::cout << "rebalancing " << key << '\n';

  auto body = std::string{};
  if (!readFromAnyRealVolume(key_path, real_volumes, body)) {
    return false;
  }

  auto write_error = false;
  for (const auto &volume : missingTargetVolumes(real_volumes, target_volumes)) {
    const auto remote = "http://" + volume + key_path;
    try {
      volume_client::remotePut(remote, body);
    } catch (const std::exception &err) {
      std::cerr << "rebalance put error " << err.what() << " " << remote
                << '\n';
      write_error = true;
    }
  }
  if (write_error) {
    return false;
  }

  if (!index.putRecord(key, record::Record{target_volumes, record::Deleted::NO,
                                           ""})) {
    std::cerr << "rebalance put db error " << key << '\n';
    return false;
  }

  auto delete_error = false;
  for (const auto &volume : staleRealVolumes(real_volumes, target_volumes)) {
    const auto remote = "http://" + volume + key_path;
    try {
      volume_client::remoteDelete(remote);
    } catch (const std::exception &err) {
      std::cerr << "rebalance delete error " << err.what() << " " << remote
                << '\n';
      delete_error = true;
    }
  }

  return !delete_error;
}

int run(const Options &options) {
  std::cout << "rebalancing to";
  for (const auto &volume : options.volumes) {
    std::cout << ' ' << volume;
  }
  std::cout << '\n';

  auto index = index::LevelDbIndex{options.db_path};
  auto ok = true;
  const auto records = index.listRecords("", "", 0);
  for (const auto &entry : records.records) {
    if (!rebalanceObject(index, options, entry.key, entry.record.rvolumes)) {
      ok = false;
    }
  }

  return ok ? 0 : 1;
}

} // namespace minikv::rebalance
