#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace minikv {

using Md5Digest = std::array<unsigned char, 16>;

Md5Digest md5(std::string_view input);
std::string md5_hex(std::string_view input);
std::string md5_hex_files(const std::vector<std::filesystem::path> &paths);

} // namespace minikv
