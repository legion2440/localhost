# Localhost — HTTP/1.1 сервер

Однопоточный неблокирующий HTTP-сервер на Rust для задания 01-edu `localhost`. Парсинг HTTP, маршрутизация, virtual hosts, загрузка файлов, cookies/sessions, CGI и Linux `epoll` реализованы самостоятельно, без готового web/async runtime.

· [English version](README.md)

## Быстрый старт

### Требования

- Linux с `epoll`
- Rust 1.75+
- Cargo
- Python 3
- `php-cgi` для второго CGI-бонуса
- компилятор C++17 для второй реализации сервера
- `siege` для официального stress test

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

Открыть `http://127.0.0.1:8080/`.

Проверить конфигурацию без открытия listener'ов:

```bash
./target/release/localhost --check-config -c localhost.conf
```

### C++ бонус

```bash
make -C bonus_cpp
./bonus_cpp/localhost_cpp -c localhost.conf
```

Обе реализации используют один конфиг, один набор static/CGI-файлов и один black-box test suite.

## Архитектура

| Модуль | Назначение |
| --- | --- |
| `src/main.rs` | CLI и запуск процесса |
| `src/config.rs` | Парсинг/валидация конфига и route matching |
| `src/http.rs` | Инкрементальный HTTP parser, chunk framing и сериализация response |
| `src/server.rs` | `epoll`, lifecycle соединений, routing, files, uploads, sessions |
| `src/cgi.rs` | Запуск CGI, неблокирующий CGI stream I/O и разбор ответа |
| `src/util.rs` | URL/path normalization, escaping и безопасные имена файлов |

Основной сервер использует один process, один thread и один экземпляр `epoll`. На одно client event выполняется максимум один socket read или один socket write. Сокеты неблокирующие, partial read/write продолжаются на следующих level-triggered событиях, `EAGAIN` / `EWOULDBLOCK` не блокируют loop. Реализованы timeout, корректный half-close и generation token для защиты от stale epoll events после повторного использования fd.

Отдельный process создаётся только для CGI. CGI stdin/stdout соединены через full-duplex Unix socket (`UnixStream::pair` в Rust, `socketpair` в C++). Родительский endpoint неблокирующий и зарегистрирован в том же `epoll`: request body пишется инкрементально, `shutdown(SHUT_WR)` передаёт обязательный EOF, stdout читается по `EPOLLIN`. Статус child process проверяется отдельно неблокирующим `try_wait` / `waitpid(..., WNOHANG)`, зависший CGI завершается по timeout.

В release-профиле Rust больше нет `panic = "abort"`; обработка epoll events защищена `catch_unwind`, поэтому неожиданный panic внутри отдельного event не должен намеренно завершать весь server process.

## Возможности HTTP

| Возможность | Реализация |
| --- | --- |
| Основной протокол | HTTP/1.1; HTTP/1.0 requests также принимаются |
| Методы | `GET`, `POST`, `DELETE` |
| Request body | `Content-Length`, `Transfer-Encoding: chunked` |
| Keep-alive | HTTP/1.1 persistent; HTTP/1.0 — только при `Connection: keep-alive` |
| Static | binary-safe чтение + MIME types |
| Index | configurable `index` |
| Directory listing | `autoindex on/off` |
| Redirect | configurable 3xx `return` |
| Upload | multipart и raw/chunked body |
| DELETE | удаление файлов по разрешённому route |
| Virtual hosts | `Host` + `server_name`, первый валидный server — default |
| Body limit | `client_max_body_size`, ранний ответ `413` |
| Errors | custom `400/403/404/405/413/500` |
| Response headers | `Content-Length`, `Date`, `Connection`, `Server`, content type |
| Sessions | `session_id` + in-memory state |
| CGI | extension mapping, `PATH_INFO`, stdin EOF и epoll-driven stream I/O |

Chunked parser хранит состояние между read-событиями, а не парсит body заново с начала. Chunk, превышающий configured body limit, отклоняется до полной буферизации payload. Данные следующего pipelined request после `0\r\n\r\n` сохраняются.

Path traversal через `..`, включая percent-encoded варианты, отклоняется до обращения к filesystem. Имя загружаемого файла приводится к безопасному basename.

## Конфигурация

Reference [`localhost.conf`](localhost.conf) демонстрирует два порта и virtual host на общем listener:

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

Поддерживаемые directives:

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

Повтор одинакового `listen` внутри одного server block считается ошибкой. Разные virtual hosts могут легально использовать один `host:port`. При этом повтор одного `server_name` на том же listener либо второй unnamed/default server на этом listener определяется как конфликтующий block и пропускается; остальные валидные server blocks продолжают работать.

Комментарии поддерживаются как небольшое расширение, хотя задание этого не требует.

Проверка virtual host:

```bash
curl --resolve alt.local:8080:127.0.0.1 http://alt.local:8080/
```

## CGI

Обязательный Python CGI:

```text
/cgi-bin/test.py
```

Бонусный PHP CGI:

```text
/cgi-bin/test.php
```

Передаются CGI variables:

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

Полностью декодированный request body передаётся в CGI stdin, после тела передаётся EOF. Chunked request сначала декодируется HTTP parser. В regression suite есть CGI POST размером больше типичной ёмкости pipe/socket buffer — он проверяет, что streaming stdin/stdout не уходит в deadlock.

Пример:

```bash
curl 'http://127.0.0.1:8080/cgi-bin/test.py/demo/path?name=Auditor'
```

## Cookies и sessions

`GET /session` создаёт или продолжает in-memory session:

```bash
curl -i 'http://127.0.0.1:8080/session?user=Auditor'
```

Response устанавливает `session_id`. Повторный request с этой cookie увеличивает счётчик посещений. Неактивные sessions удаляются через час.

## Браузерный стенд

В [`public/`](public/) находится обычный HTML/CSS/JavaScript testbed для GET/POST, multipart/streaming upload, GET/DELETE файлов, body limit, cookies/sessions, Python/PHP CGI, redirects и custom errors.

JS не пытается вручную задавать запрещённый браузером `Transfer-Encoding`; детерминированные chunked cases проверяются raw HTTP-тестами.

## Проверка

CI теперь считает Rust warnings ошибками и запускает Clippy перед функциональными тестами. Локально тот же gate:

```bash
cargo clippy --all-targets -- -D warnings
RUSTFLAGS="-D warnings" cargo test
RUSTFLAGS="-D warnings" cargo build --release
python3 tests/audit.py target/release/localhost

make -C bonus_cpp clean
make -C bonus_cpp
python3 tests/audit.py bonus_cpp/localhost_cpp
```

Общий black-box suite среди прочего проверяет:

- конфликтующие и легальные shared-listener конфигурации;
- static, оба порта и virtual hosts;
- `Date` и совместимость с HTTP/1.0 request;
- redirects, custom errors, method restrictions;
- целостность upload, DELETE и корректный `204`;
- chunked, ранний `413`, half-close и pipelining;
- sessions/cookies;
- Python/PHP CGI, `PATH_INFO`, chunked CGI POST и большой CGI streaming body;
- malformed request survival и autoindex.

### Stress

```bash
bash siege_test.sh
```

или:

```bash
siege -b http://127.0.0.1:8080/
```

Требуемая audit availability — минимум **99.5%**.

Локальный fallback:

```bash
python3 tests/stress.py 8080 1000 50
```

Для leak-check стоит мониторить RSS/file descriptors под длительной нагрузкой либо использовать подходящий memory-analysis tool.

## Бонусы

| Бонус | Реализация |
| --- | --- |
| Больше одного CGI | Python (`.py`) + PHP CGI (`.php`) |
| Вторая реализация сервера | самостоятельный C++17 server в `bonus_cpp/` |

C++-сервер не запускает Rust-бинарник и не является wrapper. У него собственные configuration parser, epoll loop, HTTP parser, uploads, sessions и CGI process/stream handling; он проходит тот же `tests/audit.py`.

## Структура проекта

```text
.
├── src/                    # Rust server
├── bonus_cpp/              # независимый C++17 bonus server
├── public/                 # browser testbed + error pages
│   ├── error_pages/
│   └── uploads/
├── public_alt/             # shared-port virtual-host demo
├── cgi-bin/                # Python + PHP CGI
├── tests/
│   ├── audit.py            # общий black-box regression suite
│   └── stress.py           # local availability fallback
├── localhost.conf
├── siege_test.sh
├── Cargo.toml
└── Makefile
```

## Примечания

- Основная реализация рассчитана на Linux из-за `epoll`.
- Client sockets и CGI parent streams неблокирующие и управляются одним epoll instance.
- Configuration и обычный local filesystem access используют стандартные filesystem API.
- Sessions хранятся в памяти и исчезают после остановки сервера.
- `siege` следует запускать только против собственных систем или при явном разрешении владельца.

## Авторы

- Atabek Furkat [**@abakhram**](https://01.tomorrow-school.ai/intra/astanahub/users/8197)
- Nazar Yestayev [**@nyestaye**](https://01.tomorrow-school.ai/intra/astanahub/users/4468)
- Sultan Yersultan [**@syersult**](https://01.tomorrow-school.ai/intra/astanahub/users/4423)
