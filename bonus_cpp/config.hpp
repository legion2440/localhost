#pragma once
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static constexpr size_t HARD_LIMIT = 32u * 1024u * 1024u;
static constexpr int MAX_EVENTS = 128;
static const auto CLIENT_TIMEOUT = std::chrono::seconds(15);

struct CgiMap {
    std::string ext;
    std::string interpreter;
};

struct Route {
    std::string path = "/";
    std::vector<std::string> methods{"GET"};
    fs::path root = ".";
    std::string index;
    bool autoindex = false;
    std::optional<std::pair<int, std::string>> redirect;
    std::vector<CgiMap> cgi;
};

struct ServerConfig {
    std::vector<std::pair<std::string, int>> listens;
    std::vector<std::string> names;
    size_t body_limit = 1024 * 1024;
    std::map<int, std::string> error_pages;
    std::vector<Route> routes;
};

struct Request {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::string version;
    std::map<std::string, std::string> headers;
    std::vector<char> body;

    std::string header(const std::string& name) const;
    bool close_requested() const;
};

struct RequestHead {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::string version;
    std::map<std::string, std::string> headers;
    size_t body_start = 0;

    std::string header(const std::string& name) const;
    std::optional<size_t> content_length() const;
    bool is_chunked() const;
    Request into_request(std::vector<char> body) const;
};

struct Response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<char> body;
};

struct ChunkProgress {
    size_t pos = 0;
    size_t decoded_len = 0;
};

struct PendingRequest {
    RequestHead head;
    size_t server_index = 0;
    ChunkProgress chunk;
};

struct Client {
    int fd = -1;
    int listener_fd = -1;
    uint32_t generation = 0;
    std::vector<char> read_buf;
    std::vector<char> write_buf;
    size_t write_pos = 0;
    bool close_after = false;
    bool peer_closed = false;
    std::optional<PendingRequest> pending;
    Clock::time_point last = Clock::now();
};

struct Listener {
    int fd = -1;
    std::string host;
    int port = 0;
    std::vector<size_t> servers;
};

struct Session {
    std::string user;
    unsigned long long visits = 0;
    Clock::time_point last = Clock::now();
};

struct CgiTask {
    size_t server_index = 0;
    pid_t pid = -1;
    std::string in_path;
    std::string out_path;
    bool keep_alive = true;
    Clock::time_point started = Clock::now();
};

static std::string trim(std::string value) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

static std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

static std::vector<std::string> split_ws(const std::string& value) {
    std::istringstream input(value);
    std::vector<std::string> output;
    std::string token;
    while (input >> token) {
        output.push_back(token);
    }
    return output;
}

static bool starts_with(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

static std::string clean_line(const std::string& raw) {
    const auto pos = raw.find('#');
    return trim(raw.substr(0, pos));
}

static size_t parse_size(std::string value) {
    size_t multiplier = 1;
    if (!value.empty() && (value.back() == 'k' || value.back() == 'K')) {
        multiplier = 1024;
        value.pop_back();
    } else if (!value.empty() && (value.back() == 'm' || value.back() == 'M')) {
        multiplier = 1024 * 1024;
        value.pop_back();
    }
    return std::stoull(value) * multiplier;
}

std::string Request::header(const std::string& name) const {
    const auto it = headers.find(lower(name));
    return it == headers.end() ? "" : it->second;
}

bool Request::close_requested() const {
    return lower(header("connection")) == "close";
}

std::string RequestHead::header(const std::string& name) const {
    const auto it = headers.find(lower(name));
    return it == headers.end() ? "" : it->second;
}

std::optional<size_t> RequestHead::content_length() const {
    const std::string value = header("content-length");
    if (value.empty()) {
        return std::nullopt;
    }
    try {
        return std::stoull(value);
    } catch (...) {
        return std::nullopt;
    }
}

bool RequestHead::is_chunked() const {
    std::stringstream input(lower(header("transfer-encoding")));
    std::string token;
    while (std::getline(input, token, ',')) {
        if (trim(token) == "chunked") {
            return true;
        }
    }
    return false;
}

Request RequestHead::into_request(std::vector<char> request_body) const {
    Request request;
    request.method = method;
    request.target = target;
    request.path = path;
    request.query = query;
    request.version = version;
    request.headers = headers;
    request.body = std::move(request_body);
    return request;
}

static std::vector<std::vector<std::string>> server_blocks(const std::string& text) {
    std::vector<std::vector<std::string>> blocks;
    std::vector<std::string> current;
    std::istringstream input(text);
    std::string raw;
    bool active = false;
    int depth = 0;

    while (std::getline(input, raw)) {
        const std::string line = clean_line(raw);
        if (line.empty()) {
            continue;
        }
        if (!active) {
            if (line != "server {") {
                throw std::runtime_error("expected `server {`");
            }
            active = true;
            depth = 1;
            current.clear();
            continue;
        }

        depth += static_cast<int>(std::count(line.begin(), line.end(), '{'));
        depth -= static_cast<int>(std::count(line.begin(), line.end(), '}'));
        if (depth == 0) {
            blocks.push_back(current);
            current.clear();
            active = false;
        } else {
            current.push_back(line);
        }
        if (depth < 0) {
            throw std::runtime_error("unmatched brace");
        }
    }

    if (active || depth != 0) {
        throw std::runtime_error("unterminated server block");
    }
    return blocks;
}

static Route parse_route(const std::string& path, const std::vector<std::string>& lines) {
    Route route;
    route.path = path;
    for (const auto& line : lines) {
        auto parts = split_ws(line);
        if (parts.empty()) {
            continue;
        }
        if (parts[0] == "methods") {
            route.methods.assign(parts.begin() + 1, parts.end());
            for (auto& method : route.methods) {
                std::transform(method.begin(), method.end(), method.begin(), [](unsigned char c) {
                    return static_cast<char>(std::toupper(c));
                });
            }
        } else if (parts[0] == "root" && parts.size() == 2) {
            route.root = parts[1];
        } else if (parts[0] == "index" && parts.size() == 2) {
            route.index = parts[1];
        } else if (parts[0] == "autoindex" && parts.size() == 2) {
            route.autoindex = parts[1] == "on";
        } else if (parts[0] == "return" && parts.size() == 3) {
            route.redirect = {{std::stoi(parts[1]), parts[2]}};
        } else if (parts[0] == "cgi_extension" && parts.size() == 3) {
            route.cgi.push_back({lower(parts[1]), parts[2]});
        } else {
            throw std::runtime_error("bad location directive: " + line);
        }
    }
    return route;
}

static ServerConfig parse_server(const std::vector<std::string>& lines) {
    ServerConfig server;
    std::set<std::string> listens;

    for (size_t i = 0; i < lines.size();) {
        const std::string line = lines[i];
        if (starts_with(line, "location ") && !line.empty() && line.back() == '{') {
            const auto parts = split_ws(line);
            if (parts.size() < 3) {
                throw std::runtime_error("bad location");
            }
            std::vector<std::string> nested;
            int depth = 1;
            ++i;
            for (; i < lines.size() && depth > 0; ++i) {
                depth += static_cast<int>(std::count(lines[i].begin(), lines[i].end(), '{'));
                depth -= static_cast<int>(std::count(lines[i].begin(), lines[i].end(), '}'));
                if (depth > 0) {
                    nested.push_back(lines[i]);
                }
            }
            if (depth != 0) {
                throw std::runtime_error("unterminated location");
            }
            server.routes.push_back(parse_route(parts[1], nested));
            continue;
        }

        const auto parts = split_ws(line);
        if (parts.empty()) {
            ++i;
            continue;
        }
        if (parts[0] == "listen" && parts.size() == 2) {
            if (!listens.insert(parts[1]).second) {
                throw std::runtime_error("duplicate listen in one server: " + parts[1]);
            }
            const auto colon = parts[1].rfind(':');
            if (colon == std::string::npos) {
                throw std::runtime_error("bad listen");
            }
            server.listens.push_back({parts[1].substr(0, colon), std::stoi(parts[1].substr(colon + 1))});
        } else if (parts[0] == "server_name" && parts.size() >= 2) {
            for (size_t j = 1; j < parts.size(); ++j) {
                server.names.push_back(lower(parts[j]));
            }
        } else if (parts[0] == "client_max_body_size" && parts.size() == 2) {
            server.body_limit = parse_size(parts[1]);
        } else if (parts[0] == "error_page" && parts.size() == 3) {
            server.error_pages[std::stoi(parts[1])] = parts[2];
        } else if (parts[0] != "}") {
            throw std::runtime_error("bad server directive: " + line);
        }
        ++i;
    }

    if (server.listens.empty() || server.routes.empty()) {
        throw std::runtime_error("server needs listen and location");
    }
    return server;
}

static std::vector<ServerConfig> load_config(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("cannot read config");
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const auto blocks = server_blocks(buffer.str());
    std::vector<ServerConfig> output;
    for (size_t i = 0; i < blocks.size(); ++i) {
        try {
            output.push_back(parse_server(blocks[i]));
        } catch (const std::exception& error) {
            std::cerr << "config warning: server #" << i + 1 << " skipped: " << error.what() << "\n";
        }
    }
    if (output.empty()) {
        throw std::runtime_error("no valid server blocks");
    }
    return output;
}

static bool route_match(const std::string& prefix, const std::string& path) {
    if (prefix == "/") {
        return !path.empty() && path.front() == '/';
    }
    if (path == prefix) {
        return true;
    }
    return starts_with(path, prefix) && path.size() > prefix.size() && path[prefix.size()] == '/';
}

static const Route* route_for(const ServerConfig& server, const std::string& path) {
    const Route* best = nullptr;
    for (const auto& route : server.routes) {
        if (route_match(route.path, path) && (!best || route.path.size() > best->path.size())) {
            best = &route;
        }
    }
    return best;
}

static const fs::path* default_root(const ServerConfig& server) {
    for (const auto& route : server.routes) {
        if (route.path == "/") {
            return &route.root;
        }
    }
    return nullptr;
}

static std::string route_relative(const Route& route, const std::string& path) {
    std::string relative = route.path == "/"
        ? path
        : (starts_with(path, route.path) ? path.substr(route.path.size()) : "");
    while (!relative.empty() && relative.front() == '/') {
        relative.erase(relative.begin());
    }
    return relative;
}
