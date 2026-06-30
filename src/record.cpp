#include "record.hpp"
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<std::string> splite(std::string_view ss, const char s) {
  std::vector<std::string> result;
  while (true) {
    auto pos = ss.find(s);
    if (pos == std::string_view::npos) {
      result.emplace_back(ss);
      return result;
    }

    result.emplace_back(ss.substr(0, pos));
    ss.remove_prefix(pos + 1);
  }
}

std::string join(const std::vector<std::string> &spliters,
                 const std::string_view joiner) {
  auto result = std::string{};
  auto size = spliters.size();
  if (size == 0) {
    return result;
  }

  for (std::size_t i = 0; i < size - 1; i++) {
    result += spliters[i];
    result += joiner;
  }

  result += spliters.back();
  return result;
}

} // namespace

record::Record record::toRecord(std::string_view data) {
  auto rec = Record{};
  rec.deleted = Deleted::NO;

  if (data.starts_with("DELETED")) {
    rec.deleted = record::Deleted::SOFT;
    data.remove_prefix(7);
  }

  if (data.starts_with("HASH")) {
    if (data.size() < 36) {
      throw std::runtime_error("invalid hash in record");
    }
    rec.hash = std::string{data.substr(4, 32)};
    data.remove_prefix(36);
  }

  rec.rvolumes = splite(data, ',');
  return rec;
}

std::string record::fromRecord(const Record &rec) {
  auto cc = std::string{};

  if (rec.deleted == Deleted::HARD) {
    throw std::runtime_error("Can't put HARD delete in the database");
  }

  if (rec.deleted == Deleted::SOFT) {
    cc = "DELETED";
  }

  if (rec.hash.size() == 32) {
    cc += "HASH" + rec.hash;
  }

  cc += join(rec.rvolumes, ",");

  return cc;
}
