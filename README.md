# Localhost — HTTP/1.1 Server

A single-threaded, non-blocking HTTP server written in Rust for the 01-edu `localhost` assignment. Request parsing, routing, virtual hosts, uploads, cookies/sessions, CGI execution and the Linux `epoll` event loop are implemented without a web or async runtime.

· [Русская версия](README_RU.md)

## Quick start

### Requirements

- Linux with `epoll`
- Rust 1.75+
- Cargo
- Python 3
- `php-cgi` for the second CGI bonus
- a C++17 compiler for the second-server bonus
- `siege` for the official stress test

Debian/Ubuntu:

```bash
sudo apt update
sudo apt install python3 php-cgi siege build-essential
```

### Rust

```bash
cargo build --release
./target/release/localhost -c localhost.conf
```

Open `http://127.0.0.1:8080/`.

Validate configuration without opening listeners:

```bash
./target/release/localhost --check-config -c localhost.conf
```

### C++ bonus

```bash
make -C bonus_cpp
./bonus_cpp/localhost_cpp -c localhost.conf
```

Both implementations use the same configuration, static files, CGI scripts and black-box test suite.

## Architecture

| Module | Responsibility |
| --- | --- |
| `src/main.rs` | CLI and process startup |
| `src/config.rs` | Configuration parsing, validation and route matching |
| `src/http.rs` | Incremental HTTP parser, chunk framing and response serialization |
| `src/server.rs` | `epoll`, connection lifecycle, routing, files, uploads and sessions |
| `src/cgi.rs` | CGI launch, non-blocking CGI stream handling and response parsing |
| `src/util.rs` | URL/path normalization, escaping and filename safety |

The main server uses one process, one thread and one `epoll` instance. A client event performs at most one socket read or one socket write. Sockets are non-blocking, partial transfers continue on later level-triggered events, and `EAGAIN` / `EWOULDBLOCK` never block the loop. Connection timeouts, half-close handling and generation-tagged epoll tokens protect the event loop from stale file-descriptor events.

CGI is the only operation that creates a child process. CGI stdin/stdout are connected through a full-duplex Unix socket (`UnixStream::pair` in Rust, `socketpair` in C++). The parent endpoint is non-blocking and registered in the same `epoll` instance. The request body is written incrementally, `shutdown(SHUT_WR)` supplies the required EOF, and CGI stdout is read incrementally from `EPOLLIN`. Child status is checked separately with non-blocking `try_wait` / `waitpid(..., WNOHANG)` and a timeout.

The Rust event loop uses unwindable release panics and guards event handling with `catch_unwind`, so an unexpected per-event panic does not intentionally abort the whole server process.

## HTTP features

| Feature | Implementation |
| --- | --- |
| Primary protocol | HTTP/1.1; HTTP/1.0 requests are also accepted |
| Methods | `GET`, `POST`, `DELETE` |
| Request bodies | `Content-Length`, `Transfer-Encoding: chunked` |
| Keep-alive | HTTP/1.1 persistent connections; HTTP/1.0 opt-in keep-alive |
| Static files | Binary-safe reads with MIME types |
| Directory index | Configurable `index` |
| Directory listing | Configurable `autoindex on/off` |
| Redirects | Configurable 3xx `return` |
| Uploads | Multipart form-data and raw/chunked bodies |
| Deletes | Route-controlled file deletion |
| Virtual hosts | `Host` + `server_name`, first valid server is default |
| Body limit | Per-server `client_max_body_size`, rejected early with `413` |
| Errors | Custom `400`, `403`, `404`, `405`, `413`, `500` pages |
| Responses | `Content-Length`, `Date`, `Connection`, `Server` and appropriate content type |
| Sessions | In-memory `session_id` cookie state |
| CGI | Extension mapping, `PATH_INFO`, stdin EOF and non-blocking epoll stream I/O |

Chunked parsing keeps incremental state instead of reparsing the complete body after each read. A chunk that would exceed the configured body limit is rejected before the entire payload is buffered. Pipelined data after the terminating chunk is preserved for the next request.

Unsafe URL traversal (`..`, including percent-encoded forms) is rejected before filesystem resolution. Uploaded filenames are reduced to a safe basename.

## Configuration

The reference [`localhost.conf`](localhost.conf) demonstrates two ports and a virtual host sharing one listener:

```text
server {
    listen 127.0.0.1:8080
    listen 127.0.0.1:8081
    server_name localhost test.local
    client_max_body_size 2m

    error_page 404 /error_pages/404.html

    location / {
        methods GET
        root ./public
        index index.html
        autoindex off
    }

    location /api/echo {
        methods POST
        root ./public
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
        cgi_extension .php ../bonus_cpp/php-cgi-wrapper
    }
}

server {
    listen 127.0.0.1:8080
    server_name alt.local
    client_max_body_size 512k

    location / {
        methods GET
        root ./public_alt
        index index.html
        autoindex off
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

An identical `listen` repeated inside one server block is rejected. Different virtual hosts may legally share the same `host:port`; however, a duplicate `server_name` on the same listener, or a second unnamed/default server on that listener, is detected as a conflicting block and skipped. Other valid server blocks remain available.

Comments are accepted as a convenience extension even though the subject does not require them.

Virtual-host example:

```bash
curl --resolve alt.local:8080:127.0.0.1 http://alt.local:8080/
```

## CGI

Required Python CGI:

```text
/cgi-bin/test.py
```

Bonus PHP CGI:

```text
/cgi-bin/test.php
```

The server supplies common CGI variables including:

- `REQUEST_METHOD`
- `REQUEST_URI`
- `QUERY_STRING`
- `PATH_INFO`
- `CONTENT_LENGTH`
- `CONTENT_TYPE`
- `HTTP_COOKIE`
- `HTTP_HOST`
- `SCRIPT_FILENAME`
- `SERVER_PORT`

The complete decoded request body reaches CGI stdin and EOF is delivered after the body. Chunked requests are decoded by the HTTP parser before CGI execution. The regression suite includes a CGI POST larger than a typical pipe/socket buffer to verify that stdin/stdout streaming does not deadlock.

Example:

```bash
curl 'http://127.0.0.1:8080/cgi-bin/test.py/demo/path?name=Auditor'
```

## Cookies and sessions

`GET /session` creates or reuses an in-memory session:

```bash
curl -i 'http://127.0.0.1:8080/session?user=Auditor'
```

The response sets `session_id`. Reusing the cookie increments the visit counter. Inactive sessions expire after one hour.

## Browser testbed

[`public/`](public/) is a plain HTML/CSS/JavaScript test site for GET/POST, multipart and streaming uploads, file GET/DELETE, body limits, sessions, Python/PHP CGI, redirects and custom errors.

Browser JavaScript does not manually set the forbidden `Transfer-Encoding` header. Deterministic chunked cases are exercised through raw HTTP tests.

## Verification

The CI uses warnings as errors and runs Clippy before the functional suites. The same checks can be run locally:

```bash
cargo clippy --all-targets -- -D warnings
RUSTFLAGS="-D warnings" cargo test
RUSTFLAGS="-D warnings" cargo build --release
python3 tests/audit.py target/release/localhost

make -C bonus_cpp clean
make -C bonus_cpp
python3 tests/audit.py bonus_cpp/localhost_cpp
```

The shared black-box suite checks, among other cases:

- duplicate/shared-listener configuration handling;
- static pages, both configured ports and virtual hosts;
- `Date` and HTTP/1.0 compatibility;
- redirects, custom errors and method restrictions;
- upload integrity, DELETE and correct `204` framing;
- chunked requests, early `413`, half-close and pipelining;
- sessions/cookies;
- Python and PHP CGI, `PATH_INFO`, chunked CGI POST and large CGI streaming;
- malformed-request survival and autoindex.

### Stress test

```bash
bash siege_test.sh
```

or:

```bash
siege -b http://127.0.0.1:8080/
```

The audit requires at least **99.5%** availability. A lightweight fallback is also available:

```bash
python3 tests/stress.py 8080 1000 50
```

For leak inspection, monitor RSS/file descriptors during sustained load or run the server under a suitable memory-analysis tool.

## Bonus features

| Bonus | Implementation |
| --- | --- |
| More than one CGI system | Python (`.py`) + PHP CGI (`.php`) |
| Server in another language | Independent C++17 implementation in `bonus_cpp/` |

The C++ server does not launch or wrap the Rust executable. It has its own configuration parser, epoll loop, HTTP parser, uploads, sessions and CGI process/stream handling, and it passes the same black-box suite.

## Project structure

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

## Notes

- Linux is the primary target because the assignment uses `epoll`.
- Client sockets and CGI parent streams are non-blocking and driven by the same epoll instance.
- Configuration and ordinary local filesystem access use normal filesystem APIs.
- Sessions are in memory and disappear when the server stops.
- Run `siege` only against systems you own or have explicit permission to stress-test.

## Authors

- Atabek Furkat [**@abakhram**](https://01.tomorrow-school.ai/intra/astanahub/users/8197)
- Nazar Yestayev [**@nyestaye**](https://01.tomorrow-school.ai/intra/astanahub/users/4468)
- Sultan Yersultan [**@syersult**](https://01.tomorrow-school.ai/intra/astanahub/users/4423)
