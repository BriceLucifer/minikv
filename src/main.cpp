#include "cli.hpp"
#include "server.hpp"

#include <httplib.h>

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  // Keep argv conversion outside the parser so the parser stays unit-testable.
  auto args = std::vector<std::string>{};
  args.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);
  for (auto i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  const auto parsed = minikv::cli::parseCommandLine(args);
  if (!parsed.ok) {
    std::cerr << parsed.error << '\n';
    return 1;
  }

  if (parsed.options.command != "server") {
    // Accepted by the parser for command-line parity, but implementation is
    // intentionally deferred until rebuild/rebalance logic is ported.
    std::cerr << parsed.options.command << " is not implemented yet\n";
    return 1;
  }

  std::cout << "volume servers:";
  for (const auto &volume : parsed.options.app.volumes) {
    std::cout << ' ' << volume;
  }
  std::cout << '\n';

  auto app = minikv::server::App{parsed.options.app};
  auto server = httplib::Server{};
  minikv::server::registerRoutes(server, app);

  if (!server.listen("0.0.0.0", parsed.options.port)) {
    std::cerr << "failed to listen on port " << parsed.options.port << '\n';
    return 1;
  }

  return 0;
}
