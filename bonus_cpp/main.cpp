#include "server.hpp"

int main(int argc, char** argv) {
    std::string config_path = "localhost.conf";
    bool check_config = false;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if ((argument == "-c" || argument == "--config") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (argument == "--check-config") {
            check_config = true;
        } else if (argument == "-h" || argument == "--help") {
            std::cout << "localhost_cpp [-c FILE] [--check-config]\n";
            return 0;
        } else {
            std::cerr << "unknown argument: " << argument << "\n";
            return 2;
        }
    }

    try {
        if (check_config) {
            std::ifstream file(config_path);
            if (!file) {
                throw std::runtime_error("cannot read config");
            }
            std::stringstream buffer;
            buffer << file.rdbuf();
            const auto blocks = server_blocks(buffer.str());
            std::vector<ServerConfig> configs;
            for (size_t i = 0; i < blocks.size(); ++i) {
                ServerConfig candidate = parse_server(blocks[i]);
                for (const auto& existing : configs) {
                    if (const auto conflict = server_conflict(existing, candidate)) {
                        throw std::runtime_error(
                            "server #" + std::to_string(i + 1) + ": " + *conflict
                        );
                    }
                }
                configs.push_back(std::move(candidate));
            }
            if (configs.empty()) {
                throw std::runtime_error("no valid server blocks");
            }
            std::cout << "configuration OK: " << configs.size() << " valid server block(s)\n";
            return 0;
        }

        auto configs = load_config(config_path);
        App app(std::move(configs));
        app.run();
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
