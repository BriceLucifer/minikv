#include <string>
#include <vector>

namespace record {

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

} // namespace record
