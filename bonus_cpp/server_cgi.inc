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

    enum class CgiFinishKind {
        Success,
        Timeout,
        Failed,
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

        int sockets[2] = {-1, -1};
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) < 0) {
            queue(fd, error(cfg[server_index], 500, "socketpair failed"), keep_alive);
            return;
        }
        const int flags = fcntl(sockets[0], F_GETFL, 0);
        if (flags < 0 || fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK) < 0) {
            close(sockets[0]);
            close(sockets[1]);
            queue(fd, error(cfg[server_index], 500, "cannot make CGI channel nonblocking"), keep_alive);
            return;
        }

        const pid_t pid = fork();
        if (pid < 0) {
            close(sockets[0]);
            close(sockets[1]);
            queue(fd, error(cfg[server_index], 500, "fork failed"), keep_alive);
            return;
        }
        if (pid == 0) {
            close(sockets[0]);
            if (dup2(sockets[1], STDIN_FILENO) < 0 || dup2(sockets[1], STDOUT_FILENO) < 0) {
                _exit(127);
            }
            close(sockets[1]);

            const fs::path absolute = fs::absolute(spec.script);
            setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
            setenv("SERVER_PROTOCOL", request.version.c_str(), 1);
            setenv("SERVER_SOFTWARE", "localhost-cpp/0.1", 1);
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

        close(sockets[1]);
        CgiTask task;
        task.server_index = server_index;
        task.pid = pid;
        task.io_fd = sockets[0];
        task.io_generation = allocate_generation();
        task.input = request.body;
        task.keep_alive = keep_alive;
        task.started = Clock::now();
        if (task.input.empty()) {
            if (shutdown(task.io_fd, SHUT_WR) < 0 && errno != ENOTCONN) {
                kill(pid, SIGKILL);
                waitpid(pid, nullptr, 0);
                close(task.io_fd);
                queue(fd, error(cfg[server_index], 500, "cannot close CGI stdin"), keep_alive);
                return;
            }
            task.input_closed = true;
        }

        uint32_t interests = EPOLLIN | EPOLLRDHUP;
        if (!task.input_closed) {
            interests |= EPOLLOUT;
        }
        try {
            ctl(
                EPOLL_CTL_ADD,
                task.io_fd,
                interests,
                event_token(task.io_fd, task.io_generation)
            );
            modify_interest(fd, EPOLLRDHUP);
        } catch (...) {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, task.io_fd, nullptr);
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            close(task.io_fd);
            remove(fd);
            return;
        }

        cgi_io[task.io_fd] = {fd, task.io_generation};
        cgis[fd] = std::move(task);
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

    void handle_cgi_io(int io_fd, uint32_t flags) {
        const auto map_it = cgi_io.find(io_fd);
        if (map_it == cgi_io.end()) {
            return;
        }
        const int client_fd = map_it->second.client_fd;
        auto task_it = cgis.find(client_fd);
        if (task_it == cgis.end()) {
            return;
        }
        CgiTask& task = task_it->second;

        if ((flags & EPOLLERR) != 0) {
            finish_cgi(client_fd, CgiFinishKind::Failed, "CGI channel error");
            return;
        }

        if ((flags & EPOLLIN) != 0 ||
            (((flags & (EPOLLRDHUP | EPOLLHUP)) != 0) && (flags & EPOLLOUT) == 0)) {
            char buffer[65536];
            const ssize_t count = recv(io_fd, buffer, sizeof(buffer), 0);
            if (count > 0) {
                task.output.insert(task.output.end(), buffer, buffer + count);
                if (task.output.size() > MAX_CGI_OUTPUT) {
                    finish_cgi(client_fd, CgiFinishKind::Failed, "CGI output too large");
                    return;
                }
            } else if (count == 0) {
                task.io_eof = true;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                finish_cgi(client_fd, CgiFinishKind::Failed, "CGI read failed");
                return;
            }
        } else if (!task.input_closed && (flags & EPOLLOUT) != 0) {
            const ssize_t count = send(
                io_fd,
                task.input.data() + static_cast<std::ptrdiff_t>(task.input_pos),
                task.input.size() - task.input_pos,
                MSG_NOSIGNAL
            );
            if (count > 0) {
                task.input_pos += static_cast<size_t>(count);
                if (task.input_pos == task.input.size()) {
                    if (shutdown(io_fd, SHUT_WR) < 0 && errno != ENOTCONN) {
                        finish_cgi(client_fd, CgiFinishKind::Failed, "CGI stdin shutdown failed");
                        return;
                    }
                    task.input_closed = true;
                }
            } else if (count == 0) {
                finish_cgi(client_fd, CgiFinishKind::Failed, "CGI stdin closed early");
                return;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                finish_cgi(client_fd, CgiFinishKind::Failed, "CGI write failed");
                return;
            }
        }

        task_it = cgis.find(client_fd);
        if (task_it == cgis.end()) {
            return;
        }
        uint32_t interests = EPOLLIN | EPOLLRDHUP;
        if (!task_it->second.input_closed) {
            interests |= EPOLLOUT;
        }
        try {
            ctl(
                EPOLL_CTL_MOD,
                io_fd,
                interests,
                event_token(io_fd, task_it->second.io_generation)
            );
        } catch (...) {
            finish_cgi(client_fd, CgiFinishKind::Failed, "cannot update CGI epoll interest");
            return;
        }

        if (task_it->second.io_eof && task_it->second.process_exited) {
            finish_cgi(client_fd, CgiFinishKind::Success, "");
        }
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
            if (Clock::now() - task.started > std::chrono::seconds(5)) {
                kill(task.pid, SIGKILL);
                waitpid(task.pid, nullptr, 0);
                task.process_exited = true;
                finish_cgi(fd, CgiFinishKind::Timeout, "CGI timeout");
                continue;
            }
            if (task.process_exited) {
                if (task.io_eof) {
                    finish_cgi(fd, CgiFinishKind::Success, "");
                }
                continue;
            }

            int status = 0;
            const pid_t result = waitpid(task.pid, &status, WNOHANG);
            if (result == 0) {
                continue;
            }
            if (result < 0) {
                finish_cgi(fd, CgiFinishKind::Failed, "waitpid failed");
                continue;
            }
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                task.process_exited = true;
                finish_cgi(fd, CgiFinishKind::Failed, "CGI failed");
                continue;
            }
            task.process_exited = true;
            if (task.io_eof) {
                finish_cgi(fd, CgiFinishKind::Success, "");
            }
        }
    }

    void finish_cgi(int client_fd, CgiFinishKind kind, const std::string& message) {
        auto it = cgis.find(client_fd);
        if (it == cgis.end()) {
            return;
        }
        CgiTask task = std::move(it->second);
        cgis.erase(it);
        cgi_io.erase(task.io_fd);
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, task.io_fd, nullptr);

        if (!task.process_exited) {
            kill(task.pid, SIGKILL);
            waitpid(task.pid, nullptr, 0);
        }
        close(task.io_fd);
        if (clients.count(client_fd) == 0) {
            return;
        }

        Response response;
        if (kind == CgiFinishKind::Timeout) {
            response = error(cfg[task.server_index], 504, "CGI timeout");
        } else if (kind == CgiFinishKind::Failed) {
            response = error(cfg[task.server_index], 502, message.empty() ? "CGI failed" : message);
        } else {
            response = parse_cgi(task.output);
        }
        queue(client_fd, response, task.keep_alive);
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
            CgiTask task = std::move(cgi_it->second);
            cgis.erase(cgi_it);
            cgi_io.erase(task.io_fd);
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, task.io_fd, nullptr);
            kill(task.pid, SIGKILL);
            waitpid(task.pid, nullptr, 0);
            close(task.io_fd);
        }
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        clients.erase(fd);
    }
};
