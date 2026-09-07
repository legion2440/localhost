# Localhost — HTTP/1.1 сервер

Однопоточный неблокирующий HTTP/1.1 сервер на Rust для задания 01-edu `localhost`. Парсинг запросов, маршрутизация, виртуальные хосты, загрузка файлов, cookies/sessions, CGI и Linux `epoll` реализованы самостоятельно, без готового async/web runtime.

· [English version](README.md)

## 📋 Содержание

- [🚀 Быстрый старт](#-быстрый-старт)
- [📝 О проекте](#-о-проекте)
- [⚙️ Архитектура](#️-архитектура)
- [🌐 Возможности HTTP](#-возможности-http)
- [🧭 Конфигурация](#-конфигурация)
- [🧩 CGI](#-cgi)
- [🍪 Cookies и sessions](#-cookies-и-sessions)
- [🖥️ Браузерный стенд](#️-браузерный-стенд)
- [🧪 Проверка](#-проверка)
- [✨ Бонусы](#-бонусы)
- [📁 Структура проекта](#-структура-проекта)
- [⚠️ Примечания](#️-примечания)
- [🧑‍💻 Авторы](#-авторы)

## 🚀 Быстрый старт

### Требования

- Linux с `epoll`
- Rust 1.75+
- Cargo
- Python 3 для обязательного CGI и тестов
- `php-cgi` для второго CGI-бонуса
- компилятор C++17 для второй реализации сервера
- `siege` для официального стресс-теста

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

Открыть:

```text
http://127.0.0.1:8080/
```

Проверить конфиг без открытия портов:

```bash
./target/release/localhost --check-config -c localhost.conf
```

### C++ бонус

```bash
make -C bonus_cpp
./bonus_cpp/localhost_cpp -c localhost.conf
```

Обе реализации используют один конфиг, один набор статических файлов/CGI и один black-box test suite.

## 📝 О проекте

Сервер напрямую принимает HTTP/1.1 соединения через неблокирующие TCP-сокеты. Не используются `tokio`, `nix`, Hyper, Actix и другие готовые серверные/runtime-реализации. В Rust-версии единственная сторонняя зависимость — `libc` для системных вызовов `epoll`.

Дефолтный конфиг демонстрирует:

- порты `8080` и `8081`;
- default virtual server для `localhost` / `test.local`;
- `alt.local` на том же `127.0.0.1:8080`;
- ограничения методов по route;
- custom error pages;
- лимит request body;
- redirects;
- autoindex;
- Python и PHP CGI.

## ⚙️ Архитектура

| Модуль | Назначение |
| --- | --- |
| `src/main.rs` | CLI и запуск процесса |
| `src/config.rs` | Парсер/валидация конфига, route matching |
| `src/http.rs` | HTTP/1.1 parser, chunk decoder, сериализация response |
| `src/server.rs` | `epoll`, lifecycle клиентов, routing, static/upload/session |
| `src/cgi.rs` | Запуск CGI, timeout и парсинг CGI response |
| `src/util.rs` | Нормализация URL/path, escaping, безопасные имена файлов |

### Event loop

Основной сервер — один process, один thread и один экземпляр `epoll`. Каждая итерация делает один `epoll_wait`, после чего обрабатывает возвращённые события.

Для client sockets:

- сокеты неблокирующие;
- read выполняется только после `EPOLLIN`;
- write выполняется только после `EPOLLOUT`;
- на одно client event приходится максимум один socket `read` или один socket `write`;
- `EAGAIN` / `EWOULDBLOCK` не блокируют цикл;
- socket error/disconnect удаляет клиента;
- зависшие/неполные соединения закрываются по timeout;
- используется level-triggered `epoll`, поэтому partial read/write продолжаются на следующих событиях.

Новый process создаётся только для CGI, что разрешено заданием. Родительский сервер при этом остаётся однопоточным: завершение CGI проверяется неблокирующим polling, а зависший CGI убивается по timeout.

## 🌐 Возможности HTTP

| Возможность | Реализация |
| --- | --- |
| HTTP | HTTP/1.1 |
| Методы | `GET`, `POST`, `DELETE` |
| Request body | `Content-Length`, `Transfer-Encoding: chunked` |
| Keep-alive | persistent connections + idle timeout |
| Static | binary-safe, MIME types |
| Index | configurable `index` |
| Directory listing | `autoindex on/off` |
| Redirect | configurable 3xx `return` |
| Upload | multipart и raw/chunked body |
| DELETE | удаление файлов по разрешённому route |
| Virtual hosts | `Host` + `server_name`, первый server — default |
| Body limit | `client_max_body_size`, ответ `413` |
| Errors | custom `400/403/404/405/413/500` |
| Sessions | `session_id` + in-memory state |
| CGI | mapping extension → interpreter + `PATH_INFO` |

Path traversal через `..`, включая percent-encoded варианты, отклоняется до обращения к filesystem. Имя загружаемого файла приводится к безопасному basename.

## 🧭 Конфигурация

Пример из [`localhost.conf`](localhost.conf):

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

Повтор одинакового `listen` внутри одного server block считается ошибкой. Несколько virtual servers могут легально использовать один listener и различаются по `Host`.

Комментарии поддержаны как небольшое расширение, хотя задание их поддержки не требует.

Проверка virtual host:

```bash
curl --resolve alt.local:8080:127.0.0.1 http://alt.local:8080/
```

## 🧩 CGI

Обязательный CGI:

```text
/cgi-bin/test.py
```

Бонусный второй CGI:

```text
/cgi-bin/test.php
```

Передаются переменные:

- `REQUEST_METHOD`
- `QUERY_STRING`
- `PATH_INFO`
- `CONTENT_LENGTH`
- `CONTENT_TYPE`
- `HTTP_COOKIE`
- `HTTP_HOST`
- `SCRIPT_FILENAME`
- `SERVER_PORT`

Body передаётся CGI через stdin с EOF после конца body. Chunked request сначала декодируется сервером.

Пример:

```bash
curl 'http://127.0.0.1:8080/cgi-bin/test.py/demo/path?name=Auditor'
```

## 🍪 Cookies и sessions

`GET /session` создаёт или продолжает серверную session:

```bash
curl -i 'http://127.0.0.1:8080/session?user=Auditor'
```

Response содержит cookie `session_id`. При следующем запросе с этой cookie увеличивается счётчик посещений. Неактивные sessions удаляются через один час.

## 🖥️ Браузерный стенд

В [`public/`](public/) находится обычный HTML/CSS/JS стенд без React/Vue и других framework.

Через него можно проверить:

- GET/POST;
- multipart upload;
- streaming upload;
- GET/DELETE загруженных файлов;
- body limit;
- cookies/sessions;
- Python/PHP CGI и `PATH_INFO`;
- redirect;
- custom error pages.

JS не пытается вручную задавать запрещённый браузером `Transfer-Encoding`. Для streaming `fetch` браузер сам выбирает framing; детерминированные chunked-проверки выполняются raw HTTP-тестами.

## 🧪 Проверка

### Unit tests

```bash
cargo test
```

### Полная black-box проверка Rust

```bash
cargo build --release
python3 tests/audit.py target/release/localhost
```

### Та же проверка C++ бонуса

```bash
make -C bonus_cpp
python3 tests/audit.py bonus_cpp/localhost_cpp
```

### Stress

Официальный вариант:

```bash
./siege_test.sh
```

или:

```bash
siege -b http://127.0.0.1:8080/
```

Требуемая availability — минимум **99.5%**.

Локальный fallback без `siege`:

```bash
python3 tests/stress.py 8080 1000 50
```

Memory/RSS/file descriptors дополнительно проверяются под длительной нагрузкой через Valgrind/top и системные инструменты.

## ✨ Бонусы

Реализованы оба направления бонусов из задания.

| Бонус | Реализация |
| --- | --- |
| Больше одного CGI | Python (`.py`) + PHP CGI (`.php`) |
| Вторая реализация сервера | самостоятельный C++17 server в `bonus_cpp/` |

C++-сервер не запускает Rust-бинарник и не является wrapper. У него собственные `epoll`, HTTP parser, routing, uploads, sessions и CGI; он проходит тот же `tests/audit.py`.

## 📁 Структура проекта

```text
.
├── src/                    # Rust server
├── bonus_cpp/              # C++17 bonus server
├── public/                 # browser testbed + error pages
│   ├── error_pages/
│   └── uploads/
├── public_alt/             # virtual-host demo
├── cgi-bin/                # Python + PHP CGI
├── tests/
│   ├── audit.py
│   └── stress.py
├── localhost.conf
├── siege_test.sh
├── Cargo.toml
└── Makefile
```

## ⚠️ Примечания

- Основная реализация рассчитана на Linux, так как используется `epoll`.
- File I/O для static/CGI temp files выполняется локально; клиентский **socket I/O** остаётся неблокирующим и управляется `epoll`.
- Sessions хранятся в памяти и исчезают после остановки сервера.
- `siege` можно запускать только против собственных систем или при явном разрешении владельца.

## 🧑‍💻 Авторы

- Atabek Furkat [**@abakhram**](https://01.tomorrow-school.ai/intra/astanahub/users/8197)
- Nazar Yestayev [**@nyestaye**](https://01.tomorrow-school.ai/intra/astanahub/users/4468)
- Sultan Yersultan [**@syersult**](https://01.tomorrow-school.ai/intra/astanahub/users/4423)
