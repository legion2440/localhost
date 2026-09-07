    size_t default_server(int fd) const {
        const auto client_it = clients.find(fd);
        if (client_it == clients.end()) {
            return 0;
        }
        const auto listener_it = listeners.find(client_it->second.listener_fd);
        if (listener_it == listeners.end() || listener_it->second.servers.empty()) {
            return 0;
        }
        return listener_it->second.servers.front();
    }

    size_t select_server(int fd, const std::string& host_header) const {
        const auto client_it = clients.find(fd);
        if (client_it == clients.end()) {
            return 0;
        }
        const auto listener_it = listeners.find(client_it->second.listener_fd);
        if (listener_it == listeners.end() || listener_it->second.servers.empty()) {
            return 0;
        }

        std::string host = trim(host_header);
        const auto colon = host.find(':');
        if (colon != std::string::npos) {
            host = host.substr(0, colon);
        }
        host = lower(host);
        for (size_t index : listener_it->second.servers) {
            for (const auto& name : cfg[index].names) {
                if (lower(name) == host) {
                    return index;
                }
            }
        }
        return listener_it->second.servers.front();
    }

    void process(int fd) {
        auto client_it = clients.find(fd);
        if (client_it == clients.end()) {
            return;
        }

        if (!client_it->second.pending) {
            HeadParseOut parsed_head = parse_request_head(client_it->second.read_buf);
            if (parsed_head.kind == ParseKind::NeedMore) {
                return;
            }
            if (parsed_head.kind == ParseKind::Error) {
                queue(fd, error(cfg[default_server(fd)], 400, parsed_head.error), false);
                return;
            }
            if (parsed_head.kind == ParseKind::TooLarge) {
                queue(fd, error(cfg[default_server(fd)], 413, parsed_head.error), false);
                return;
            }

            const size_t server_index = select_server(fd, parsed_head.head.header("host"));
            if (const auto length = parsed_head.head.content_length()) {
                if (*length > cfg[server_index].body_limit) {
                    queue(
                        fd,
                        error(cfg[server_index], 413, "request body exceeds configured limit"),
                        false
                    );
                    return;
                }
            }
            PendingRequest pending;
            pending.head = std::move(parsed_head.head);
            pending.server_index = server_index;
            client_it->second.pending = std::move(pending);
        }

        client_it = clients.find(fd);
        if (client_it == clients.end() || !client_it->second.pending) {
            return;
        }
        PendingRequest& pending = *client_it->second.pending;
        const size_t server_index = pending.server_index;
        BodyParseOut parsed_body = parse_request_body(
            client_it->second.read_buf,
            pending.head,
            pending.chunk,
            cfg[server_index].body_limit
        );
        if (parsed_body.kind == ParseKind::NeedMore) {
            return;
        }
        if (parsed_body.kind == ParseKind::Error) {
            client_it->second.pending.reset();
            queue(fd, error(cfg[server_index], 400, parsed_body.error), false);
            return;
        }
        if (parsed_body.kind == ParseKind::TooLarge) {
            client_it->second.pending.reset();
            queue(fd, error(cfg[server_index], 413, parsed_body.error), false);
            return;
        }

        if (parsed_body.consumed > client_it->second.read_buf.size()) {
            remove(fd);
            return;
        }
        RequestHead head = client_it->second.pending->head;
        client_it->second.pending.reset();
        client_it->second.read_buf.erase(
            client_it->second.read_buf.begin(),
            client_it->second.read_buf.begin() + static_cast<std::ptrdiff_t>(parsed_body.consumed)
        );
        Request request = head.into_request(std::move(parsed_body.body));

        const auto normalized = normalize_path(request.path);
        if (!normalized) {
            queue(fd, error(cfg[server_index], 400, "invalid path"), false);
            return;
        }
        request.path = *normalized;
        const Route* route = route_for(cfg[server_index], *normalized);
        if (!route) {
            queue(
                fd,
                error(cfg[server_index], 404, "no route"),
                !request.close_requested()
            );
            return;
        }
        handle(fd, server_index, cfg[server_index], *route, request);
    }

    bool allowed(const Route& route, const std::string& method) const {
        return std::find(route.methods.begin(), route.methods.end(), method) != route.methods.end();
    }

    void handle(
        int fd,
        size_t server_index,
        const ServerConfig& server,
        const Route& route,
        const Request& request
    ) {
        const bool keep_alive = !request.close_requested();
        if (!allowed(route, request.method)) {
            Response response = error(server, 405, "method not allowed");
            std::string allow;
            for (size_t i = 0; i < route.methods.size(); ++i) {
                if (i != 0) {
                    allow += ", ";
                }
                allow += route.methods[i];
            }
            response.headers.push_back({"Allow", allow});
            queue(fd, response, keep_alive);
            return;
        }

        if (route.redirect) {
            Response response;
            response.status = route.redirect->first;
            response.headers.push_back({"Location", route.redirect->second});
            queue(fd, response, keep_alive);
            return;
        }

        if (const auto spec = find_cgi(route, request.path)) {
            start_cgi(fd, server_index, *spec, request, keep_alive);
            return;
        }

        if (request.path == "/api/echo" && request.method == "POST") {
            Response response;
            response.status = 200;
            response.body = request.body;
            const std::string content_type = request.header("content-type");
            response.headers.push_back({
                "Content-Type",
                content_type.empty() ? "application/octet-stream" : content_type,
            });
            queue(fd, response, keep_alive);
            return;
        }

        if (request.path == "/session" && request.method == "GET") {
            queue(fd, session(request), keep_alive);
            return;
        }

        if (route.path == "/uploads" && request.method == "POST") {
            queue(fd, upload(server, route, request), keep_alive);
            return;
        }
        if (request.method == "DELETE") {
            queue(fd, del(server, route, request.path), keep_alive);
            return;
        }
        if (request.method == "GET") {
            queue(fd, get(server, route, request.path), keep_alive);
            return;
        }

        queue(fd, error(server, 405, "unsupported method"), keep_alive);
    }

    Response error(const ServerConfig& server, int status, const std::string& message) const {
        const auto page_it = server.error_pages.find(status);
        const fs::path* root = default_root(server);
        if (page_it != server.error_pages.end() && root) {
            try {
                const size_t first = page_it->second.find_first_not_of('/');
                const std::string relative = first == std::string::npos ? "" : page_it->second.substr(first);
                const fs::path path = safe_join(*root, relative);
                if (fs::is_regular_file(path)) {
                    Response response;
                    response.status = status;
                    response.body = read_file(path);
                    response.headers.push_back({"Content-Type", mime(path)});
                    return response;
                }
            } catch (...) {
            }
        }
        return text_response(
            status,
            "<!doctype html><html><body><h1>" + std::to_string(status)
                + "</h1><p>" + html_escape(message) + "</p></body></html>",
            "text/html; charset=utf-8"
        );
    }

    Response get(const ServerConfig& server, const Route& route, const std::string& path) const {
        fs::path target;
        try {
            target = safe_join(route.root, route_relative(route, path));
        } catch (...) {
            return error(server, 403, "unsafe path");
        }

        if (fs::is_regular_file(target)) {
            Response response;
            response.status = 200;
            response.body = read_file(target);
            response.headers.push_back({"Content-Type", mime(target)});
            return response;
        }
        if (!fs::is_directory(target)) {
            return error(server, 404, "not found");
        }

        if (!route.index.empty() && fs::is_regular_file(target / route.index)) {
            const fs::path index = target / route.index;
            Response response;
            response.status = 200;
            response.body = read_file(index);
            response.headers.push_back({"Content-Type", mime(index)});
            return response;
        }
        if (!route.autoindex) {
            return error(server, 403, "autoindex disabled");
        }

        std::vector<fs::directory_entry> entries;
        for (const auto& entry : fs::directory_iterator(target)) {
            entries.push_back(entry);
        }
        std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
            return left.path().filename() < right.path().filename();
        });

        std::string html = "<!doctype html><html><body><h1>Index</h1><ul>";
        const std::string base = !path.empty() && path.back() == '/' ? path : path + "/";
        for (const auto& entry : entries) {
            const std::string name = entry.path().filename().string();
            if (!name.empty() && name.front() == '.') {
                continue;
            }
            const std::string suffix = entry.is_directory() ? "/" : "";
            html += "<li><a href=\"" + base + url_path_escape(name) + suffix + "\">"
                + html_escape(name) + suffix + "</a></li>";
        }
        html += "</ul></body></html>";
        return text_response(200, html, "text/html; charset=utf-8");
    }

    std::optional<std::pair<std::string, std::vector<char>>> multipart(const Request& request) const {
        const std::string content_type = request.header("content-type");
        const std::string tag = "boundary=";
        const auto boundary_pos = content_type.find(tag);
        if (boundary_pos == std::string::npos) {
            return std::nullopt;
        }

        std::string boundary = content_type.substr(boundary_pos + tag.size());
        const auto semicolon = boundary.find(';');
        if (semicolon != std::string::npos) {
            boundary = boundary.substr(0, semicolon);
        }
        boundary = trim(boundary);
        if (boundary.size() >= 2 && boundary.front() == '"' && boundary.back() == '"') {
            boundary = boundary.substr(1, boundary.size() - 2);
        }
        const std::string marker = "--" + boundary;
        size_t pos = 0;

        for (;;) {
            const auto start = find_bytes(request.body, pos, marker);
            if (!start) {
                return std::nullopt;
            }
            size_t section = *start + marker.size();
            if (section + 2 <= request.body.size()
                && std::string(
                    request.body.begin() + static_cast<std::ptrdiff_t>(section),
                    request.body.begin() + static_cast<std::ptrdiff_t>(section + 2)
                ) == "--") {
                return std::nullopt;
            }
            if (section + 2 > request.body.size()) {
                return std::nullopt;
            }
            section += 2;

            const auto header_end = find_bytes(request.body, section, "\r\n\r\n");
            if (!header_end) {
                return std::nullopt;
            }
            const std::string headers(
                request.body.begin() + static_cast<std::ptrdiff_t>(section),
                request.body.begin() + static_cast<std::ptrdiff_t>(*header_end)
            );
            const auto filename_pos = headers.find("filename=\"");
            if (filename_pos != std::string::npos) {
                const size_t filename_start = filename_pos + 10;
                const auto filename_end = headers.find('"', filename_start);
                if (filename_end == std::string::npos) {
                    return std::nullopt;
                }
                const std::string name = sanitize_filename(
                    headers.substr(filename_start, filename_end - filename_start)
                );
                const size_t data_start = *header_end + 4;
                const auto data_end = find_bytes(request.body, data_start, "\r\n--" + boundary);
                if (!data_end) {
                    return std::nullopt;
                }
                return std::make_pair(
                    name,
                    std::vector<char>(
                        request.body.begin() + static_cast<std::ptrdiff_t>(data_start),
                        request.body.begin() + static_cast<std::ptrdiff_t>(*data_end)
                    )
                );
            }
            pos = *header_end + 4;
        }
    }

    Response upload(const ServerConfig& server, const Route& route, const Request& request) const {
        std::error_code error_code;
        fs::create_directories(route.root, error_code);

        std::string name = route_relative(route, request.path);
        std::vector<char> body = request.body;
        if (starts_with(lower(request.header("content-type")), "multipart/form-data")) {
            auto parsed = multipart(request);
            if (!parsed) {
                return error(server, 400, "bad multipart upload");
            }
            name = parsed->first;
            body = std::move(parsed->second);
        } else if (name.empty()) {
            name = request.header("x-filename");
            if (name.empty()) {
                name = "payload.bin";
            }
        }

        name = sanitize_filename(name);
        if (name.empty()) {
            return error(server, 400, "bad filename");
        }
        const fs::path target = route.root / name;
        if (!write_file(target, body)) {
            return error(server, 500, "write failed");
        }
        Response response = text_response(201, "uploaded " + name + "\n");
        response.headers.push_back({"Location", route.path + "/" + name});
        return response;
    }

    Response del(const ServerConfig& server, const Route& route, const std::string& path) const {
        const std::string relative = route_relative(route, path);
        if (relative.empty()) {
            return error(server, 403, "refuse route root");
        }
        fs::path target;
        try {
            target = safe_join(route.root, relative);
        } catch (...) {
            return error(server, 403, "unsafe path");
        }
        if (!fs::exists(target)) {
            return error(server, 404, "not found");
        }
        if (!fs::is_regular_file(target)) {
            return error(server, 403, "not a file");
        }
        std::error_code error_code;
        fs::remove(target, error_code);
        if (error_code) {
            return error(server, 403, "delete failed");
        }
        Response response;
        response.status = 204;
        return response;
    }
