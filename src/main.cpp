#include "cli.hpp"
#include "rebalance.hpp"
#include "rebuild.hpp"
#include "server.hpp"
#include "volume_client.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  try {
    // Keep argv conversion outside the parser so the parser stays
    // unit-testable.
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

    if (parsed.options.command == "rebuild") {
      return minikv::rebuild::run(minikv::rebuild::Options{
          .db_path = parsed.options.app.db_path,
          .volumes = parsed.options.app.volumes,
          .replicas = parsed.options.app.replicas,
          .subvolumes = parsed.options.app.subvolumes,
      });
    }

    if (parsed.options.command == "rebalance") {
      return minikv::rebalance::run(minikv::rebalance::Options{
          .db_path = parsed.options.app.db_path,
          .volumes = parsed.options.app.volumes,
          .replicas = parsed.options.app.replicas,
          .subvolumes = parsed.options.app.subvolumes,
      });
    }

    if (parsed.options.command != "server") {
      std::cerr << parsed.options.command << " is not implemented yet\n";
      return 1;
    }

    std::cout << "volume servers:";
    for (const auto &volume : parsed.options.app.volumes) {
      std::cout << ' ' << volume;
    }
    std::cout << '\n';

    auto app = minikv::server::App{parsed.options.app};
    minikv::volume_client::setConnectionPoolWidth(
        parsed.options.app.volume_connection_pool);
    auto server = minikv::http::Server{};
    minikv::server::registerRoutes(server, app);

    if (!server.listen("0.0.0.0", parsed.options.port)) {
      std::cerr << "failed to listen on port " << parsed.options.port << '\n';
      return 1;
    }

    return 0;
  } catch (const std::exception &ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
