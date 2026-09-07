class App {
public:
    explicit App(std::vector<ServerConfig> configs)
        : cfg(std::move(configs)) {
        epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd < 0) {
            throw std::runtime_error(strerror(errno));
        }
        bind_all();
    }

    ~App() {
        for (auto& [fd, client] : clients) {
            (void)client;
            close(fd);
        }
        for (auto& [fd, listener] : listeners) {
            (void)listener;
            close(fd);
        }
        if (epoll_fd >= 0) {
            close(epoll_fd);
        }
    }

    void run() {
        std::vector<epoll_event> events(MAX_EVENTS);
        for (;;) {
            const int count = epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), 100);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(strerror(errno));
            }

            poll_cgi();

            for (int i = 0; i < count; ++i) {
                const uint64_t token = events[static_cast<size_t>(i)].data.u64;
                const int fd = token_fd(token);
                const uint32_t generation = token_generation(token);
                const uint32_t flags = events[static_cast<size_t>(i)].events;

                if (generation == 0) {
                    if (listeners.count(fd) != 0) {
                        accept_one(fd);
                    }
                    continue;
                }

                const auto client_it = clients.find(fd);
                if (client_it == clients.end() || client_it->second.generation != generation) {
                    continue;
                }

                if ((flags & EPOLLERR) != 0) {
                    remove(fd);
                    continue;
                }
                if ((flags & (EPOLLRDHUP | EPOLLHUP)) != 0) {
                    clients[fd].peer_closed = true;
                }

                if (cgis.count(fd) != 0) {
                    continue;
                }

                const bool writing = clients[fd].write_pos < clients[fd].write_buf.size();
                if (writing && (flags & EPOLLOUT) != 0) {
                    write_one(fd);
                } else if ((flags & EPOLLIN) != 0) {
                    read_one(fd);
                } else if (clients.count(fd) != 0 && clients[fd].peer_closed) {
                    finish_peer_eof(fd);
                }
            }

            poll_cgi();
            expire();
        }
    }

private:
    int epoll_fd = -1;
    std::vector<ServerConfig> cfg;
    std::unordered_map<int, Listener> listeners;
    std::unordered_map<int, Client> clients;
    std::unordered_map<int, CgiTask> cgis;
    std::unordered_map<std::string, Session> sessions;
    unsigned long long seq = 0;
    uint32_t next_generation = 0;

    static uint64_t event_token(int fd, uint32_t generation) {
        return (static_cast<uint64_t>(generation) << 32)
            | static_cast<uint32_t>(fd);
    }

    static int token_fd(uint64_t token) {
        return static_cast<int>(static_cast<uint32_t>(token));
    }

    static uint32_t token_generation(uint64_t token) {
        return static_cast<uint32_t>(token >> 32);
    }

    uint32_t allocate_generation() {
        do {
            ++next_generation;
        } while (next_generation == 0);
        return next_generation;
    }

    void ctl(int operation, int fd, uint32_t events, uint64_t token) {
        epoll_event event{};
        event.events = events;
        event.data.u64 = token;
        if (epoll_ctl(epoll_fd, operation, fd, &event) < 0) {
            throw std::runtime_error(strerror(errno));
        }
    }

    void modify_interest(int fd, uint32_t events) {
        const auto it = clients.find(fd);
        if (it == clients.end()) {
            throw std::runtime_error("client no longer exists");
        }
        ctl(EPOLL_CTL_MOD, fd, events, event_token(fd, it->second.generation));
    }

    void bind_all() {
        std::map<std::pair<std::string, int>, std::vector<size_t>> groups;
        for (size_t i = 0; i < cfg.size(); ++i) {
            for (const auto& address : cfg[i].listens) {
                groups[address].push_back(i);
            }
        }

        for (const auto& [address, indices] : groups) {
            const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (fd < 0) {
                continue;
            }
            int one = 1;
            (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

            sockaddr_in socket_address{};
            socket_address.sin_family = AF_INET;
            socket_address.sin_port = htons(static_cast<uint16_t>(address.second));
            if (inet_pton(AF_INET, address.first.c_str(), &socket_address.sin_addr) != 1
                || bind(fd, reinterpret_cast<sockaddr*>(&socket_address), sizeof(socket_address)) < 0
                || listen(fd, 256) < 0) {
                std::cerr << "listener " << address.first << ':' << address.second
                          << " skipped: " << strerror(errno) << "\n";
                close(fd);
                continue;
            }

            ctl(EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLRDHUP, event_token(fd, 0));
            listeners[fd] = {fd, address.first, address.second, indices};
            std::cerr << "listening on " << address.first << ':' << address.second << "\n";
        }

        if (listeners.empty()) {
            throw std::runtime_error("no listener bound");
        }
    }

    void accept_one(int listener_fd) {
        sockaddr_in address{};
        socklen_t address_len = sizeof(address);
        const int fd = accept4(
            listener_fd,
            reinterpret_cast<sockaddr*>(&address),
            &address_len,
            SOCK_NONBLOCK | SOCK_CLOEXEC
        );
        if (fd < 0) {
            return;
        }

        int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        const uint32_t generation = allocate_generation();
        try {
            ctl(
                EPOLL_CTL_ADD,
                fd,
                EPOLLIN | EPOLLRDHUP,
                event_token(fd, generation)
            );
        } catch (...) {
            close(fd);
            return;
        }

        Client client;
        client.fd = fd;
        client.listener_fd = listener_fd;
        client.generation = generation;
        clients[fd] = std::move(client);
    }

    void read_one(int fd) {
        char buffer[65536];
        const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count == 0) {
            const auto it = clients.find(fd);
            if (it != clients.end()) {
                it->second.peer_closed = true;
            }
            finish_peer_eof(fd);
            return;
        }
        if (count < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                remove(fd);
            }
            return;
        }

        auto it = clients.find(fd);
        if (it == clients.end()) {
            return;
        }
        it->second.read_buf.insert(it->second.read_buf.end(), buffer, buffer + count);
        it->second.last = Clock::now();
        process(fd);
    }

    void finish_peer_eof(int fd) {
        auto it = clients.find(fd);
        if (it == clients.end() || cgis.count(fd) != 0) {
            return;
        }
        if (it->second.write_pos < it->second.write_buf.size()) {
            return;
        }
        if (it->second.read_buf.empty() && !it->second.pending) {
            remove(fd);
            return;
        }

        process(fd);
        it = clients.find(fd);
        if (it == clients.end() || cgis.count(fd) != 0) {
            return;
        }
        if (it->second.write_pos >= it->second.write_buf.size()) {
            queue(fd, error(cfg[default_server(fd)], 400, "incomplete request before EOF"), false);
        }
    }

    void write_one(int fd) {
        auto it = clients.find(fd);
        if (it == clients.end()) {
            return;
        }
        Client& client = it->second;
        const ssize_t count = send(
            fd,
            client.write_buf.data() + static_cast<std::ptrdiff_t>(client.write_pos),
            client.write_buf.size() - client.write_pos,
            MSG_NOSIGNAL
        );
        if (count <= 0) {
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            remove(fd);
            return;
        }

        client.write_pos += static_cast<size_t>(count);
        client.last = Clock::now();
        if (client.write_pos != client.write_buf.size()) {
            return;
        }

        const bool buffered = !client.read_buf.empty();
        const bool close_after = client.close_after || (client.peer_closed && !buffered);
        client.write_buf.clear();
        client.write_pos = 0;
        client.close_after = false;
        if (close_after) {
            remove(fd);
            return;
        }

        try {
            modify_interest(fd, EPOLLIN | EPOLLRDHUP);
        } catch (...) {
            remove(fd);
            return;
        }
        if (buffered) {
            process(fd);
        }
    }
