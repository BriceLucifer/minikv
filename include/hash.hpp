#pragma once

#include <array>
#include <string>
#include <string_view>

namespace minikv {

using Md5Digest = std::array<unsigned char, 16>;

Md5Digest md5(std::string_view input);
std::string md5_hex(std::string_view input);

} // namespace minikv
