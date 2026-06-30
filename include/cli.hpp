#pragma once

#include "server.hpp"

#include <string>
#include <vector>

namespace minikv::cli {

struct CommandLineOptions {
  std::string command;
  int port = 3000;
  bool verbose = false;
  server::AppOptions app;
};

struct ParseResult {
  bool ok = false;
  CommandLineOptions options;
  std::string error;
};

ParseResult parseCommandLine(const std::vector<std::string> &args);
std::string usage();

} // namespace minikv::cli
