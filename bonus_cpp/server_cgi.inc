    static std::string cookie(const std::string& header, const std::string& key) {
        std::stringstream input(header);
        std::string part;
        while (std::getline(input, part, ';')) {
            const auto equal = part.find('=');
            if (equal != std::string::npos && trim(part.substr(0, equal)) == key) {
                return trim(part.substr(equal + 1));
            }
        }
        return "";
    }

    static std::string query_value(const std::string& query, const std::string& key) {
        std::stringstream input(query);
        std::string part;
        while (std::getline(input, part, '&')) {
            const auto equal = part.find('=');
            const std::string name = equal == std::string::npos ? part : part.substr(0, equal);
            if (name == key) {
                return equal == std::string::npos ? "" : part.substr(equal + 1);
            }
        }
        return "";
    }

    Response session(const Request& request) {
        std::string id = cookie(request.header("cookie"), "session_id");
        if (id.empty()) {
            id = std::to_string(getpid()) + "-" + std::to_string(++seq) + "-"
                + std::to_string(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        Clock::now().time_since_epoch()
                    ).count()
                );
        }
        const std::string user = query_value(request.query, "user");
        Session& value = sessions[id];
        if (!user.empty()) {
            value.user = user;
        }
        if (value.user.empty()) {
            value.user = "auditor";
        }
        ++value.visits;
        value.last = Clock::now();

        const std::string body = "{\"session_id\":\"" + id + "\",\"user\":\""
            + value.user + "\",\"visits\":" + std::to_string(value.visits) + "}";
        Response response = text_response(200, body, "application/json; charset=utf-8");
        response.headers.push_back({
            "Set-Cookie",
            "session_id=" + id + "; Path=/; Max-Age=3600; SameSite=Lax",
        });
        return response;
    }

    struct CgiSpec {
        std::string interpreter;
        fs::path script;
        std::string path_info;
    };

    std::optional<CgiSpec> find_cgi(const Route& route, const std::string& path) const {
        if (route.cgi.empty()) {
            return std::nullopt;
        }
        const std::string relative = route_relative(route, path);
        std::stringstream input(relative);
        std::string segment;
        std::string script;
        std::vector<std::string> all;
        while (std::getline(input, segment, '/')) {
            if (!segment.empty()) {
                all.push_back(segment);
            }
        }

        for (size_t i = 0; i < all.size(); ++i) {
            if (!script.empty()) {
                script += '/';
            }
            script += all[i];
            const std::string segment_lower = lower(all[i]);
            for (const auto& mapping : route.cgi) {
                if (segment_lower.size() >= mapping.ext.size()
                    && segment_lower.substr(segment_lower.size() - mapping.ext.size()) == mapping.ext) {
                    std::string path_info;
                    for (size_t j = i + 1; j < all.size(); ++j) {
                        path_info += "/" + all[j];
                    }
                    return CgiSpec{mapping.interpreter, route.root / script, path_info};
                }
            }
        }
        return std::nullopt;
    }

    void start_cgi(
        int fd,
        size_t server_index,
        const CgiSpec& spec,
        const Request& request,
        bool keep_alive
    ) {
        if (!fs::is_regular_file(spec.script)) {
            queue(fd, error(cfg[server_index], 404, "CGI script not found"), keep_alive);
            return;
        }

        const std::string base = "/tmp/localhost-cpp-cgi-" + std::to_string(getpid()) + "-"
            + std::to_string(fd) + "-" + std::to_string(++seq);
        const std::string input_path = base + ".in";
        const std::string output_path = base + ".out";
        {
            std::ofstream file(input_path, std::ios::binary);
            file.write(request.body.data(), static_cast<std::streamsize>(request.body.size()));
        }

        const pid_t pid = fork();
        if (pid < 0) {
            queue(fd, error(cfg[server_index], 500, "fork failed"), keep_alive);
            return;
        }
        if (pid == 0) {
            const int input_fd = open(input_path.c_str(), O_RDONLY);
            const int output_fd = open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (input_fd < 0 || output_fd < 0) {
                _exit(127);
            }
            if (dup2(input_fd, STDIN_FILENO) < 0 || dup2(output_fd, STDOUT_FILENO) < 0) {
                _exit(127);
            }
            close(input_fd);
            close(output_fd);

            const fs::path absolute = fs::absolute(spec.script);
            setenv("REQUEST_METHOD", request.method.c_str(), 1);
            setenv("QUERY_STRING", request.query.c_str(), 1);
            setenv("PATH_INFO", spec.path_info.c_str(), 1);
            setenv("CONTENT_LENGTH", std::to_string(request.body.size()).c_str(), 1);
            setenv("CONTENT_TYPE", request.header("content-type").c_str(), 1);
            setenv("HTTP_COOKIE", request.header("cookie").c_str(), 1);
            setenv("HTTP_HOST", request.header("host").c_str(), 1);
            setenv("SCRIPT_FILENAME", absolute.c_str(), 1);
            chdir(absolute.parent_path().c_str());
            execl(
                spec.interpreter.c_str(),
                spec.interpreter.c_str(),
                absolute.filename().c_str(),
                static_cast<char*>(nullptr)
            );
            _exit(127);
        }

        try {
            modify_interest(fd, EPOLLRDHUP);
        } catch (...) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            unlink(input_path.c_str());
            unlink(output_path.c_str());
            remove(fd);
            return;
        }
        cgis[fd] = {server_index, pid, input_path, output_path, keep_alive, Clock::now()};
    }

    Response parse_cgi(const std::vector<char>& bytes) const {
        const std::string text(bytes.begin(), bytes.end());
        size_t separator = text.find("\r\n\r\n");
        size_t skip = 4;
        if (separator == std::string::npos) {
            separator = text.find("\n\n");
            skip = 2;
        }
        if (separator == std::string::npos) {
            return text_response(502, "bad CGI output");
        }

        const std::string headers = text.substr(0, separator);
        std::istringstream input(headers);
        std::string line;
        Response response;
        response.status = 200;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const auto colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            const std::string key = trim(line.substr(0, colon));
            const std::string value = trim(line.substr(colon + 1));
            if (lower(key) == "status") {
                response.status = std::stoi(value);
            } else {
                response.headers.push_back({key, value});
            }
        }
        response.body.assign(
            text.begin() + static_cast<std::ptrdiff_t>(separator + skip),
            text.end()
        );
        return response;
    }

    void poll_cgi() {
        std::vector<int> fds;
        for (const auto& [fd, task] : cgis) {
            (void)task;
            fds.push_back(fd);
        }

        for (int fd : fds) {
            auto it = cgis.find(fd);
            if (it == cgis.end()) {
                continue;
            }
            CgiTask& task = it->second;
            int status = 0;
            pid_t result = waitpid(task.pid, &status, WNOHANG);
            const bool timeout = Clock::now() - task.started > std::chrono::seconds(5);
            if (result == 0 && !timeout) {
                continue;
            }
            if (timeout && result == 0) {
                kill(task.pid, SIGKILL);
                waitpid(task.pid, &status, 0);
            }

            const CgiTask finished = task;
            cgis.erase(it);
            const std::vector<char> output = read_file(finished.out_path);
            unlink(finished.in_path.c_str());
            unlink(finished.out_path.c_str());
            if (clients.count(fd) == 0) {
                continue;
            }

            Response response;
            if (timeout) {
                response = error(cfg[finished.server_index], 504, "CGI timeout");
            } else if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                response = error(cfg[finished.server_index], 502, "CGI failed");
            } else {
                response = parse_cgi(output);
            }
            queue(fd, response, finished.keep_alive);
        }
    }

    void queue(int fd, const Response& response, bool keep_alive) {
        auto it = clients.find(fd);
        if (it == clients.end()) {
            return;
        }
        const bool effective_keep_alive = keep_alive
            && (!it->second.peer_closed || !it->second.read_buf.empty());
        it->second.write_buf = serialize(response, effective_keep_alive);
        it->second.write_pos = 0;
        it->second.close_after = !effective_keep_alive;
        it->second.last = Clock::now();
        try {
            modify_interest(fd, EPOLLOUT | EPOLLRDHUP);
        } catch (...) {
            remove(fd);
        }
    }

    void expire() {
        std::vector<int> dead;
        for (const auto& [fd, client] : clients) {
            if (cgis.count(fd) == 0 && Clock::now() - client.last > CLIENT_TIMEOUT) {
                dead.push_back(fd);
            }
        }
        for (int fd : dead) {
            remove(fd);
        }

        for (auto it = sessions.begin(); it != sessions.end();) {
            if (Clock::now() - it->second.last > std::chrono::hours(1)) {
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }
    }

    void remove(int fd) {
        const auto cgi_it = cgis.find(fd);
        if (cgi_it != cgis.end()) {
            kill(cgi_it->second.pid, SIGKILL);
            waitpid(cgi_it->second.pid, nullptr, 0);
            unlink(cgi_it->second.in_path.c_str());
            unlink(cgi_it->second.out_path.c_str());
            cgis.erase(cgi_it);
        }
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        clients.erase(fd);
    }
};
