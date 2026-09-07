static std::string html_escape(std::string value) {
    auto replace_all = [&](const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = value.find(from, pos)) != std::string::npos) {
            value.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace_all("&", "&amp;");
    replace_all("<", "&lt;");
    replace_all(">", "&gt;");
    replace_all("\"", "&quot;");
    return value;
}

static std::string url_path_escape(const std::string& value) {
    static constexpr char HEX[] = "0123456789ABCDEF";
    std::string output;
    for (unsigned char byte : value) {
        if (std::isalnum(byte) || byte == '-' || byte == '_' || byte == '.' || byte == '~') {
            output.push_back(static_cast<char>(byte));
        } else {
            output.push_back('%');
            output.push_back(HEX[(byte >> 4) & 0x0f]);
            output.push_back(HEX[byte & 0x0f]);
        }
    }
    return output;
}

static std::string reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 504: return "Gateway Timeout";
        default: return "Unknown";
    }
}

static std::string mime(const fs::path& path) {
    const std::string extension = lower(path.extension().string());
    if (extension == ".html" || extension == ".htm") return "text/html; charset=utf-8";
    if (extension == ".css") return "text/css; charset=utf-8";
    if (extension == ".js") return "application/javascript; charset=utf-8";
    if (extension == ".json") return "application/json; charset=utf-8";
    if (extension == ".txt") return "text/plain; charset=utf-8";
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".svg") return "image/svg+xml";
    if (extension == ".gif") return "image/gif";
    if (extension == ".webp") return "image/webp";
    if (extension == ".ico") return "image/x-icon";
    if (extension == ".pdf") return "application/pdf";
    return "application/octet-stream";
}

static std::vector<char> read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

static bool write_file(const fs::path& path, const std::vector<char>& bytes) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

static std::string sanitize_filename(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    const auto pos = value.rfind('/');
    if (pos != std::string::npos) {
        value = value.substr(pos + 1);
    }
    std::string output;
    for (unsigned char c : value) {
        output += (std::isalnum(c) || c == '.' || c == '-' || c == '_') ? static_cast<char>(c) : '_';
    }
    if (output.empty() || output == "." || output == "..") {
        return "";
    }
    return output;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static std::optional<std::string> normalize_path(const std::string& input) {
    if (input.empty() || input.front() != '/') {
        return std::nullopt;
    }
    std::string decoded;
    for (size_t i = 0; i < input.size();) {
        if (input[i] == '%') {
            if (i + 2 >= input.size()) {
                return std::nullopt;
            }
            const int high = hex_value(input[i + 1]);
            const int low = hex_value(input[i + 2]);
            if (high < 0 || low < 0) {
                return std::nullopt;
            }
            decoded.push_back(static_cast<char>(high * 16 + low));
            i += 3;
        } else {
            decoded.push_back(input[i++]);
        }
    }

    std::stringstream input_stream(decoded);
    std::string segment;
    std::vector<std::string> segments;
    while (std::getline(input_stream, segment, '/')) {
        if (segment.empty() || segment == ".") {
            continue;
        }
        if (segment == "..") {
            return std::nullopt;
        }
        segments.push_back(segment);
    }

    std::string output = "/";
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i != 0) {
            output += '/';
        }
        output += segments[i];
    }
    return output;
}

static fs::path safe_join(const fs::path& root, const std::string& relative) {
    fs::path path = root;
    const fs::path rel(relative);
    for (const auto& component : rel) {
        const std::string value = component.string();
        if (value == ".." || value == "/") {
            throw std::runtime_error("unsafe path");
        }
        if (value != ".") {
            path /= component;
        }
    }
    return path;
}

static std::optional<size_t> find_bytes(
    const std::vector<char>& bytes,
    size_t start,
    const std::string& needle
) {
    if (needle.empty()) {
        return std::min(start, bytes.size());
    }
    const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(std::min(start, bytes.size()));
    const auto it = std::search(begin, bytes.end(), needle.begin(), needle.end());
    if (it == bytes.end()) {
        return std::nullopt;
    }
    return static_cast<size_t>(it - bytes.begin());
}
