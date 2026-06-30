#include "base64.hpp"

#include <cstddef>
#include <stdexcept>

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

std::string base64_decode(std::string_view input) {
  if (input.size() % 4 != 0) {
    throw std::invalid_argument("invalid base64 length");
  }

  auto decode_char = [](const char ch) -> unsigned char {
    if (ch >= 'A' && ch <= 'Z') {
      return static_cast<unsigned char>(ch - 'A');
    }
    if (ch >= 'a' && ch <= 'z') {
      return static_cast<unsigned char>(ch - 'a' + 26);
    }
    if (ch >= '0' && ch <= '9') {
      return static_cast<unsigned char>(ch - '0' + 52);
    }
    if (ch == '+') {
      return 62;
    }
    if (ch == '/') {
      return 63;
    }
    throw std::invalid_argument("invalid base64 character");
  };

  auto out = std::string{};
  out.reserve((input.size() / 4) * 3);

  for (std::size_t i = 0; i < input.size(); i += 4) {
    const auto c0 = decode_char(input[i]);
    const auto c1 = decode_char(input[i + 1]);
    const auto has_c2 = input[i + 2] != '=';
    const auto has_c3 = input[i + 3] != '=';
    const auto c2 = has_c2 ? decode_char(input[i + 2]) : 0;
    const auto c3 = has_c3 ? decode_char(input[i + 3]) : 0;

    if (!has_c2 && has_c3) {
      throw std::invalid_argument("invalid base64 padding");
    }
    if ((input[i] == '=') || (input[i + 1] == '=')) {
      throw std::invalid_argument("invalid base64 padding");
    }
    if ((!has_c2 || !has_c3) && i + 4 != input.size()) {
      throw std::invalid_argument("invalid base64 padding");
    }

    out.push_back(static_cast<char>((c0 << 2) | (c1 >> 4)));
    if (has_c2) {
      out.push_back(static_cast<char>(((c1 & 0x0f) << 4) | (c2 >> 2)));
    }
    if (has_c3) {
      out.push_back(static_cast<char>(((c2 & 0x03) << 6) | c3));
    }
  }

  return out;
}

} // namespace minikv
