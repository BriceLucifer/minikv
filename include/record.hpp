#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace minikv::record {

enum class Deleted {
  NO,
  SOFT,
  HARD,
};

struct Record {
  std::vector<std::string> rvolumes;
  Deleted deleted;
  std::string hash;
};

Record toRecord(std::string_view data);
std::string fromRecord(const Record &rec);

} // namespace minikv::record
