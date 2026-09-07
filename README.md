# Localhost — HTTP/1.1 Server

A single-threaded, non-blocking HTTP/1.1 web server written in Rust for the 01-edu `localhost` assignment. It implements its own request parsing, routing, virtual hosts, uploads, cookies/sessions, CGI execution and Linux `epoll` event loop without an async web framework.

· [Русская версия](README_RU.md)

## 📋 TOC

- [🚀 Quick start](#-quick-start)
- [📝 About](#-about)
- [⚙️ Architecture](#️-architecture)
- [🌐 HTTP features](#-http-features)
- [🧭 Configuration](#-configuration)
- [🧩 CGI](#-cgi)
- [🍪 Cookies and sessions](#-cookies-and-sessions)
- [🖥️ Browser testbed](#️-browser-testbed)
- [🧪 Verification](#-verification)
- [✨ Bonus features](#-bonus-features)
- [📁 Project structure](#-project-structure)
- [⚠️ Notes](#️-notes)
- [🧑‍💻 Authors](#-authors)

## 🚀 Quick start

### Requirements

- Linux with `epoll`
- Rust 1.75 or newer
- Cargo
- Python 3 for the required CGI example and test suite
- PHP CGI (`php-cgi`) for the second CGI bonus
- a C++17 compiler for the second-server bonus
- `siege` for the official stress test

On Debian/Ubuntu, optional runtime tools can be installed with:

```bash
sudo apt update
sudo apt install python3 php-cgi siege build-essential
```

### Build and run the Rust server

```bash
cargo build --release
./target/release/localhost -c localhost.conf
```

Open:

```text
http://127.0.0.1:8080/
```

Validate a configuration without binding sockets:

```bash
./target/release/localhost --check-config -c localhost.conf
```

### Build and run the C++ bonus server

```bash
make -C bonus_cpp
./bonus_cpp/localhost_cpp -c localhost.conf
```

Both implementations use the same configuration, static files, CGI scripts and black-box test suite.

## 📝 About

The server accepts HTTP/1.1 connections directly on non-blocking TCP sockets. It does not use `tokio`, `nix`, Hyper, Actix, or another server/runtime implementation. The Rust server uses only the standard library plus `libc` for `epoll` system calls.

The supplied configuration demonstrates:

- two listening ports (`8080`, `8081`);
- a default virtual server for `localhost` / `test.local`;
- a second virtual host (`alt.local`) sharing `127.0.0.1:8080`;
- route-specific method policies;
- custom error pages;
- request-body limits;
- redirects;
- directory listing;
- Python and PHP CGI mappings.

## ⚙️ Architecture

The Rust implementation is split into focused modules:

| Module | Responsibility |
| --- | --- |
| `src/main.rs` | CLI and process startup |
| `src/config.rs` | Configuration parser, validation and route matching |
| `src/http.rs` | Incremental HTTP/1.1 parser, chunk decoder and response serialization |
| `src/server.rs` | `epoll` loop, client lifecycle, routing, static files, uploads and sessions |
| `src/cgi.rs` | CGI process launch, timeout handling and CGI response parsing |
| `src/util.rs` | URL/path normalization, escaping and filename safety |

### Event loop

The main server process uses one thread and one `epoll` instance. Each loop performs one `epoll_wait`, then handles the returned events.

For client sockets:

- every accepted socket is non-blocking;
- reads happen only after `EPOLLIN`;
- writes happen only after `EPOLLOUT`;
- one client event performs at most one socket `read` or one socket `write`;
- `EAGAIN` / `EWOULDBLOCK` are handled without blocking;
- socket errors and disconnects remove the client;
- idle/partial connections are timed out;
- level-triggered `epoll` allows partial reads/writes to continue on later events.

CGI is the only feature allowed to create another process. The parent server remains single-threaded: CGI children are polled asynchronously with `try_wait`/`waitpid(WNOHANG)` and killed after a timeout.

## 🌐 HTTP features

| Feature | Implementation |
| --- | --- |
| HTTP version | HTTP/1.1 |
| Methods | `GET`, `POST`, `DELETE` |
| Request bodies | `Content-Length` and `Transfer-Encoding: chunked` |
| Keep-alive | HTTP/1.1 persistent connections, with idle timeout |
| Static files | Binary-safe reads with MIME types |
| Directory index | Configurable `index` file |
| Directory listing | Configurable `autoindex on/off` |
| Redirects | Configurable 3xx `return` |
| Uploads | Multipart form-data and raw/chunked bodies |
| Deletes | Route-controlled file deletion |
| Virtual hosts | `Host` + `server_name`, first server is default |
| Body limit | Per-server `client_max_body_size` with `413` |
| Errors | Custom `400`, `403`, `404`, `405`, `413`, `500` pages |
| Cookies/sessions | Server-generated `session_id`, in-memory session state |
| CGI | Extension-to-interpreter mapping with `PATH_INFO` |

Unsafe URL traversal (`..`, including percent-encoded forms) is rejected before filesystem resolution. Uploaded filenames are reduced to a safe basename.

## 🧭 Configuration

The default [`localhost.conf`](localhost.conf) uses an NGINX-like subset:

```text
server {
    listen 127.0.0.1:8080
    server_name localhost test.local
    client_max_body_size 2m

    error_page 404 /error_pages/404.html

    location / {
        methods GET POST
        root ./public
        index index.html
        autoindex off
    }

    location /uploads {
        methods GET POST DELETE
        root ./public/uploads
        autoindex on
    }

    location /cgi-bin {
        methods GET POST
        root ./cgi-bin
        cgi_extension .py /usr/bin/python3
        cgi_extension .php /usr/bin/php-cgi
    }
}
```

Supported directives:

| Scope | Directive |
| --- | --- |
| server | `listen host:port` |
| server | `server_name name...` |
| server | `client_max_body_size N`, `Nk`, `Nm` |
| server | `error_page CODE /path` |
| location | `methods METHOD...` |
| location | `root PATH` |
| location | `index FILE` |
| location | `autoindex on/off` |
| location | `return STATUS TARGET` |
| location | `cgi_extension .ext INTERPRETER` |

A repeated identical `listen` inside one server block is rejected. Multiple virtual servers may intentionally share the same listener and are selected by `Host`.

Comments are accepted as a convenience extension even though the subject does not require comment support.

Test virtual-host selection with:

```bash
curl --resolve alt.local:8080:127.0.0.1 http://alt.local:8080/
```

## 🧩 CGI

The required CGI implementation is Python:

```text
/cgi-bin/test.py
```

The bonus CGI implementation is PHP:

```text
/cgi-bin/test.php
```

For a CGI request the server supplies common CGI variables including:

- `REQUEST_METHOD`
- `QUERY_STRING`
- `PATH_INFO`
- `CONTENT_LENGTH`
- `CONTENT_TYPE`
- `HTTP_COOKIE`
- `HTTP_HOST`
- `SCRIPT_FILENAME`
- `SERVER_PORT`

The request body is provided to the child as stdin and reaches EOF after the body. Both `Content-Length` and chunked requests are decoded before CGI execution.

Example:

```bash
curl 'http://127.0.0.1:8080/cgi-bin/test.py/demo/path?name=Auditor'
```

## 🍪 Cookies and sessions

`GET /session` creates or reuses a server-side session and returns JSON:

```bash
curl -i 'http://127.0.0.1:8080/session?user=Auditor'
```

The response sets a `session_id` cookie. Reusing that cookie increments the session visit counter. Sessions are kept in memory and expire after one hour of inactivity.

## 🖥️ Browser testbed

[`public/`](public/) contains a plain HTML/CSS/JavaScript test site. No frontend framework is required.

It provides controls for:

- ordinary GET and POST requests;
- multipart uploads;
- browser streaming uploads for HTTP/1.1 chunked testing;
- uploaded-file GET/DELETE operations;
- body-limit checks;
- cookies and sessions;
- Python/PHP CGI execution and `PATH_INFO`;
- redirects and custom error pages.

The browser controls never set `Transfer-Encoding` manually. For streaming `fetch` bodies the browser owns that header and, on HTTP/1.1, may choose chunked framing. The deterministic raw chunked cases are covered by the black-box test suite.

## 🧪 Verification

### Unit tests

```bash
cargo test
```

### Rust black-box suite

```bash
cargo build --release
python3 tests/audit.py target/release/localhost
```

The suite checks static files, both ports, virtual hosts, redirects, custom errors, method restrictions, POST, multipart upload integrity, DELETE, chunked bodies, sessions, CGI, body limits, malformed requests and autoindex.

### C++ bonus black-box suite

```bash
make -C bonus_cpp
python3 tests/audit.py bonus_cpp/localhost_cpp
```

### Stress test

Official `siege` command:

```bash
./siege_test.sh
```

or directly:

```bash
siege -b http://127.0.0.1:8080/
```

The required availability is at least **99.5%**.

A lightweight local fallback is also included:

```bash
python3 tests/stress.py 8080 1000 50
```

For leak inspection, run the server under tools such as Valgrind or monitor RSS/file descriptors during a sustained test.

## ✨ Bonus features

Both official bonus directions are implemented.

| Bonus | Implementation |
| --- | --- |
| More than one CGI system | Python (`.py`) + PHP CGI (`.php`) |
| Server in another language | Independent C++17 implementation in `bonus_cpp/` |

The C++ implementation is not a wrapper around the Rust executable. It has its own `epoll` loop, HTTP parser, routing, uploads, sessions and CGI process handling, and is exercised by the same black-box suite.

## 📁 Project structure

```text
.
├── src/                    # Rust server
├── bonus_cpp/              # Independent C++17 bonus server
├── public/                 # Browser testbed and custom error pages
│   ├── error_pages/
│   └── uploads/
├── public_alt/             # Shared-port virtual-host demo
├── cgi-bin/                # Python + PHP CGI scripts
├── tests/
│   ├── audit.py            # Shared black-box regression suite
│   └── stress.py           # Local availability fallback
├── localhost.conf          # Reference configuration
├── siege_test.sh
├── Cargo.toml
└── Makefile
```

## ⚠️ Notes

- The primary implementation targets Linux because the assignment explicitly requires `epoll` or an equivalent API.
- Static-file and CGI temporary-file operations use the local filesystem; client **socket** I/O remains non-blocking and event-driven.
- Sessions are intentionally in-memory and disappear when the server stops.
- Run `siege` only against systems you own or have explicit permission to stress-test.

## 🧑‍💻 Authors

- Atabek Furkat [**@abakhram**](https://01.tomorrow-school.ai/intra/astanahub/users/8197)
- Nazar Yestayev [**@nyestaye**](https://01.tomorrow-school.ai/intra/astanahub/users/4468)
- Sultan Yersultan [**@syersult**](https://01.tomorrow-school.ai/intra/astanahub/users/4423)
