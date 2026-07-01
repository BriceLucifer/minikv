#include "hash.hpp"

#include <openssl/evp.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace minikv {

namespace {

struct EvpMdCtxDeleter {
  void operator()(EVP_MD_CTX* ctx) const {
    EVP_MD_CTX_free(ctx);
  }
};

using EvpMdCtx = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;

} // namespace

Md5Digest md5(std::string_view input) {
  EvpMdCtx ctx{EVP_MD_CTX_new()};
  if (!ctx) {
    throw std::runtime_error("EVP_MD_CTX_new failed");
  }

  if (EVP_DigestInit_ex(ctx.get(), EVP_md5(), nullptr) != 1) {
    throw std::runtime_error("EVP_DigestInit_ex failed");
  }

  if (EVP_DigestUpdate(ctx.get(), input.data(), input.size()) != 1) {
    throw std::runtime_error("EVP_DigestUpdate failed");
  }

  Md5Digest digest{};
  unsigned int length = 0;
  if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &length) != 1) {
    throw std::runtime_error("EVP_DigestFinal_ex failed");
  }
  if (length != digest.size()) {
    throw std::runtime_error("unexpected MD5 digest length");
  }

  return digest;
}

std::string md5_hex(std::string_view input) {
  const auto digest = md5(input);

  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    out << std::setw(2) << static_cast<int>(byte);
  }
  return out.str();
}

std::string md5_hex_files(const std::vector<std::filesystem::path> &paths) {
  EvpMdCtx ctx{EVP_MD_CTX_new()};
  if (!ctx) {
    throw std::runtime_error("EVP_MD_CTX_new failed");
  }

  if (EVP_DigestInit_ex(ctx.get(), EVP_md5(), nullptr) != 1) {
    throw std::runtime_error("EVP_DigestInit_ex failed");
  }

  auto buffer = std::array<char, 64 * 1024>{};
  for (const auto &path : paths) {
    auto file = std::ifstream{path, std::ios::binary};
    if (!file) {
      throw std::runtime_error("failed to open file for MD5");
    }

    while (file) {
      file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const auto count = file.gcount();
      if (count <= 0) {
        continue;
      }
      if (EVP_DigestUpdate(ctx.get(), buffer.data(),
                           static_cast<std::size_t>(count)) != 1) {
        throw std::runtime_error("EVP_DigestUpdate failed");
      }
    }
  }

  auto digest = Md5Digest{};
  unsigned int length = 0;
  if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &length) != 1) {
    throw std::runtime_error("EVP_DigestFinal_ex failed");
  }
  if (length != digest.size()) {
    throw std::runtime_error("unexpected MD5 digest length");
  }

  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    out << std::setw(2) << static_cast<int>(byte);
  }
  return out.str();
}

} // namespace minikv
