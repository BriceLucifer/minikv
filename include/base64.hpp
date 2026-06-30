#pragma once

#include <string>
#include <string_view>

namespace minikv {

std::string base64_encode(std::string_view input);
std::string base64_decode(std::string_view input);

} // namespace minikv
