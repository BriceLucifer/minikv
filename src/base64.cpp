#include "base64.hpp"

#include <cstddef>

namespace minikv {

std::string base64_encode(std::string_view input) {
  static constexpr char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string out;
  out.reserve(((input.size() + 2) / 3) * 4);

  for (std::size_t i = 0; i < input.size(); i += 3) {
    const auto b0 = static_cast<unsigned char>(input[i]);
    const auto has_b1 = i + 1 < input.size();
    const auto has_b2 = i + 2 < input.size();
    const auto b1 = has_b1 ? static_cast<unsigned char>(input[i + 1]) : 0;
    const auto b2 = has_b2 ? static_cast<unsigned char>(input[i + 2]) : 0;

    out.push_back(table[b0 >> 2]);
    out.push_back(table[((b0 & 0x03) << 4) | (b1 >> 4)]);
    out.push_back(has_b1 ? table[((b1 & 0x0f) << 2) | (b2 >> 6)] : '=');
    out.push_back(has_b2 ? table[b2 & 0x3f] : '=');
  }

  return out;
}

} // namespace minikv
