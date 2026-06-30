#include "hash.hpp"

#include <openssl/evp.h>

#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

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

} // namespace minikv
