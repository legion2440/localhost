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
        auto configs = load_config(config_path);
        if (check_config) {
            std::cout << "configuration OK: " << configs.size() << " valid server block(s)\n";
            return 0;
        }
        App app(std::move(configs));
        app.run();
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
