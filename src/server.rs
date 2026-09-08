use crate::cgi::{
    cgi_error_response, parse_cgi_output, spawn_cgi, CgiLaunch, CgiSpec, CgiTask,
};
use crate::config::{RouteConfig, ServerConfig};
use crate::http::{
    mime_type, try_parse_request_body, try_parse_request_head, BodyParseResult, ChunkProgress,
    HeadParseResult, Request, RequestHead, Response,
};
use crate::util::{html_escape, normalize_url_path, safe_join, sanitize_filename};
use std::collections::HashMap;
use std::fs;
use std::io::{self, Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::os::fd::{AsRawFd, RawFd};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;
use std::time::{Duration, Instant};

const MAX_HARD_REQUEST: usize = 32 * 1024 * 1024;
const MAX_CGI_OUTPUT: usize = 32 * 1024 * 1024;
const CLIENT_TIMEOUT: Duration = Duration::from_secs(15);
const MAX_EVENTS: usize = 128;

struct ListenerEntry {
    listener: TcpListener,
    addr: SocketAddr,
    server_indices: Vec<usize>,
}

struct PendingRequest {
    head: RequestHead,
    server_index: usize,
    chunk_progress: ChunkProgress,
}

struct Client {
    stream: TcpStream,
    listener_fd: RawFd,
    generation: u32,
    read_buf: Vec<u8>,
    write_buf: Vec<u8>,
    write_pos: usize,
    close_after_write: bool,
    peer_closed: bool,
    pending: Option<PendingRequest>,
    last_active: Instant,
}

struct Session {
    username: String,
    visits: u64,
    last_seen: Instant,
}

struct CgiIoEntry {
    client_fd: RawFd,
    generation: u32,
}

enum RequestAction {
    Response(Response, bool),
    Cgi {
        spec: CgiSpec,
        request: Box<Request>,
        keep_alive: bool,
        server_index: usize,
    },
}

pub struct HttpServer {
    epoll_fd: RawFd,
    configs: Vec<ServerConfig>,
    listeners: HashMap<RawFd, ListenerEntry>,
    clients: HashMap<RawFd, Client>,
    cgi_tasks: HashMap<RawFd, CgiTask>,
    cgi_io: HashMap<RawFd, CgiIoEntry>,
    sessions: HashMap<String, Session>,
    session_seq: u64,
    next_generation: u32,
}

impl HttpServer {
    pub fn new(configs: Vec<ServerConfig>) -> io::Result<Self> {
        let epoll_fd = unsafe { libc::epoll_create1(libc::EPOLL_CLOEXEC) };
        if epoll_fd < 0 {
            return Err(io::Error::last_os_error());
        }
        let mut server = Self {
            epoll_fd,
            configs,
            listeners: HashMap::new(),
            clients: HashMap::new(),
            cgi_tasks: HashMap::new(),
            cgi_io: HashMap::new(),
            sessions: HashMap::new(),
            session_seq: 0,
            next_generation: 0,
        };
        if let Err(err) = server.bind_listeners() {
            unsafe { libc::close(epoll_fd) };
            return Err(err);
        }
        Ok(server)
    }

    fn bind_listeners(&mut self) -> io::Result<()> {
        let mut grouped: HashMap<SocketAddr, Vec<usize>> = HashMap::new();
        for (server_index, config) in self.configs.iter().enumerate() {
            for addr in &config.listens {
                grouped.entry(*addr).or_default().push(server_index);
            }
        }

        for (addr, server_indices) in grouped {
            match TcpListener::bind(addr) {
                Ok(listener) => {
                    listener.set_nonblocking(true)?;
                    let fd = listener.as_raw_fd();
                    self.epoll_add_listener(fd, (libc::EPOLLIN | libc::EPOLLRDHUP) as u32)?;
                    eprintln!("listening on {addr}");
                    self.listeners.insert(
                        fd,
                        ListenerEntry {
                            listener,
                            addr,
                            server_indices,
                        },
                    );
                }
                Err(err) => {
                    eprintln!("listener {addr} skipped: {err}");
                }
            }
        }

        if self.listeners.is_empty() {
            return Err(io::Error::new(
                io::ErrorKind::AddrNotAvailable,
                "no configured listener could be bound",
            ));
        }
        Ok(())
    }

    pub fn run(&mut self) -> io::Result<()> {
        let mut events = vec![libc::epoll_event { events: 0, u64: 0 }; MAX_EVENTS];
        loop {
            let count = unsafe {
                libc::epoll_wait(
                    self.epoll_fd,
                    events.as_mut_ptr(),
                    events.len() as i32,
                    100,
                )
            };
            if count < 0 {
                let err = io::Error::last_os_error();
                if err.kind() == io::ErrorKind::Interrupted {
                    continue;
                }
                return Err(err);
            }

            self.guard_maintenance();
            for event in events.iter().take(count as usize) {
                let token = event.u64;
                let flags = event.events;
                if catch_unwind(AssertUnwindSafe(|| self.handle_epoll_event(token, flags))).is_err() {
                    eprintln!("recovered from panic while handling epoll event");
                    self.recover_event(token);
                }
            }
            self.guard_maintenance();
        }
    }

    fn guard_maintenance(&mut self) {
        if catch_unwind(AssertUnwindSafe(|| {
            self.poll_cgi_tasks();
            self.expire_clients();
            self.expire_sessions();
        }))
        .is_err()
        {
            eprintln!("recovered from panic during server maintenance");
        }
    }

    fn handle_epoll_event(&mut self, token: u64, flags: u32) {
        let fd = token_fd(token);
        let generation = token_generation(token);

        if generation == 0 {
            if self.listeners.contains_key(&fd) {
                self.accept_once(fd);
            }
            return;
        }

        if self
            .cgi_io
            .get(&fd)
            .map(|entry| entry.generation == generation)
            .unwrap_or(false)
        {
            self.handle_cgi_io(fd, flags);
            return;
        }

        let current_generation = self.clients.get(&fd).map(|client| client.generation);
        if current_generation != Some(generation) {
            return;
        }

        if flags & libc::EPOLLERR as u32 != 0 {
            self.remove_client(fd);
            return;
        }

        if flags & (libc::EPOLLRDHUP | libc::EPOLLHUP) as u32 != 0 {
            if let Some(client) = self.clients.get_mut(&fd) {
                client.peer_closed = true;
            }
        }

        if self.cgi_tasks.contains_key(&fd) {
            return;
        }

        let wants_write = self
            .clients
            .get(&fd)
            .map(|client| client.write_pos < client.write_buf.len())
            .unwrap_or(false);
        if wants_write && flags & libc::EPOLLOUT as u32 != 0 {
            self.write_once(fd);
        } else if flags & libc::EPOLLIN as u32 != 0 {
            self.read_once(fd);
        } else if self
            .clients
            .get(&fd)
            .map(|client| client.peer_closed)
            .unwrap_or(false)
        {
            self.finish_peer_eof(fd);
        }
    }

    fn recover_event(&mut self, token: u64) {
        let fd = token_fd(token);
        let generation = token_generation(token);
        if let Some(entry) = self.cgi_io.get(&fd) {
            if entry.generation == generation {
                let client_fd = entry.client_fd;
                self.remove_client(client_fd);
            }
            return;
        }
        if self
            .clients
            .get(&fd)
            .map(|client| client.generation == generation)
            .unwrap_or(false)
        {
            self.remove_client(fd);
        }
    }

    fn accept_once(&mut self, listener_fd: RawFd) {
        let accepted = match self.listeners.get(&listener_fd) {
            Some(entry) => entry.listener.accept(),
            None => return,
        };
        let (stream, _) = match accepted {
            Ok(value) => value,
            Err(err) if err.kind() == io::ErrorKind::WouldBlock => return,
            Err(err) => {
                eprintln!("accept error: {err}");
                return;
            }
        };
        if let Err(err) = stream.set_nonblocking(true) {
            eprintln!("failed to set client nonblocking mode: {err}");
            return;
        }
        let _ = stream.set_nodelay(true);
        let fd = stream.as_raw_fd();
        let generation = self.allocate_generation();
        if let Err(err) = self.epoll_add_client(
            fd,
            generation,
            (libc::EPOLLIN | libc::EPOLLRDHUP) as u32,
        ) {
            eprintln!("failed to register client: {err}");
            return;
        }
        self.clients.insert(
            fd,
            Client {
                stream,
                listener_fd,
                generation,
                read_buf: Vec::with_capacity(8192),
                write_buf: Vec::new(),
                write_pos: 0,
                close_after_write: false,
                peer_closed: false,
                pending: None,
                last_active: Instant::now(),
            },
        );
    }

    fn read_once(&mut self, fd: RawFd) {
        let mut buf = [0u8; 65536];
        let result = match self.clients.get_mut(&fd) {
            Some(client) => client.stream.read(&mut buf),
            None => return,
        };
        match result {
            Ok(0) => {
                if let Some(client) = self.clients.get_mut(&fd) {
                    client.peer_closed = true;
                }
                self.finish_peer_eof(fd);
            }
            Ok(count) => {
                if let Some(client) = self.clients.get_mut(&fd) {
                    client.read_buf.extend_from_slice(&buf[..count]);
                    client.last_active = Instant::now();
                }
                self.process_client_buffer(fd);
            }
            Err(err) if err.kind() == io::ErrorKind::WouldBlock => {}
            Err(_) => self.remove_client(fd),
        }
    }

    fn finish_peer_eof(&mut self, fd: RawFd) {
        let idle = self
            .clients
            .get(&fd)
            .map(|client| client.write_pos >= client.write_buf.len())
            .unwrap_or(false)
            && !self.cgi_tasks.contains_key(&fd);
        if !idle {
            return;
        }

        let has_input = self
            .clients
            .get(&fd)
            .map(|client| !client.read_buf.is_empty() || client.pending.is_some())
            .unwrap_or(false);
        if !has_input {
            self.remove_client(fd);
            return;
        }

        self.process_client_buffer(fd);
        if !self.clients.contains_key(&fd) {
            return;
        }
        let still_idle = self
            .clients
            .get(&fd)
            .map(|client| client.write_pos >= client.write_buf.len())
            .unwrap_or(false)
            && !self.cgi_tasks.contains_key(&fd);
        if still_idle {
            let server_index = self.default_server_for_client(fd).unwrap_or(0);
            let server = self.configs.get(server_index).cloned();
            let response = server
                .as_ref()
                .map(|config| self.error_response(config, 400, "incomplete request before EOF"))
                .unwrap_or_else(|| cgi_error_response(400, "incomplete request before EOF"));
            self.queue_response(fd, response, false);
        }
    }

    fn write_once(&mut self, fd: RawFd) {
        let result = match self.clients.get_mut(&fd) {
            Some(client) => {
                let slice = &client.write_buf[client.write_pos..];
                client.stream.write(slice)
            }
            None => return,
        };

        match result {
            Ok(0) => self.remove_client(fd),
            Ok(count) => {
                let mut finished = false;
                let mut close = false;
                let mut has_buffered_request = false;
                if let Some(client) = self.clients.get_mut(&fd) {
                    client.write_pos += count;
                    client.last_active = Instant::now();
                    if client.write_pos >= client.write_buf.len() {
                        finished = true;
                        has_buffered_request = !client.read_buf.is_empty();
                        close = client.close_after_write
                            || (client.peer_closed && !has_buffered_request);
                        client.write_buf.clear();
                        client.write_pos = 0;
                        client.close_after_write = false;
                    }
                }
                if finished {
                    if close {
                        self.remove_client(fd);
                    } else {
                        if self
                            .modify_interest(fd, (libc::EPOLLIN | libc::EPOLLRDHUP) as u32)
                            .is_err()
                        {
                            self.remove_client(fd);
                            return;
                        }
                        if has_buffered_request {
                            self.process_client_buffer(fd);
                        }
                    }
                }
            }
            Err(err) if err.kind() == io::ErrorKind::WouldBlock => {}
            Err(_) => self.remove_client(fd),
        }
    }

    fn process_client_buffer(&mut self, fd: RawFd) {
        if !self.clients.contains_key(&fd) {
            return;
        }

        let needs_head = self
            .clients
            .get(&fd)
            .map(|client| client.pending.is_none())
            .unwrap_or(false);
        if needs_head {
            let head_result = match self.clients.get(&fd) {
                Some(client) => try_parse_request_head(&client.read_buf, MAX_HARD_REQUEST),
                None => return,
            };
            let head = match head_result {
                HeadParseResult::NeedMore => return,
                HeadParseResult::Error(message) => {
                    let server_index = self.default_server_for_client(fd).unwrap_or(0);
                    let server = self.configs.get(server_index).cloned();
                    let response = server
                        .as_ref()
                        .map(|config| self.error_response(config, 400, &message))
                        .unwrap_or_else(|| cgi_error_response(400, &message));
                    self.queue_response(fd, response, false);
                    return;
                }
                HeadParseResult::TooLarge(message) => {
                    let server_index = self.default_server_for_client(fd).unwrap_or(0);
                    let server = self.configs.get(server_index).cloned();
                    let response = server
                        .as_ref()
                        .map(|config| self.error_response(config, 413, &message))
                        .unwrap_or_else(|| cgi_error_response(413, &message));
                    self.queue_response(fd, response, false);
                    return;
                }
                HeadParseResult::Complete(head) => head,
            };

            let server_index = match self.resolve_server_for_request(fd, head.host()) {
                Some(index) => index,
                None => {
                    self.remove_client(fd);
                    return;
                }
            };
            let body_limit = self.configs[server_index].client_max_body_size;
            if head
                .content_length()
                .map(|length| length > body_limit)
                .unwrap_or(false)
            {
                let server = self.configs[server_index].clone();
                let response =
                    self.error_response(&server, 413, "request body exceeds configured limit");
                self.queue_response(fd, response, false);
                return;
            }

            if let Some(client) = self.clients.get_mut(&fd) {
                client.pending = Some(PendingRequest {
                    head,
                    server_index,
                    chunk_progress: ChunkProgress::default(),
                });
            } else {
                return;
            }
        }

        let (server_index, body_limit) = match self.clients.get(&fd) {
            Some(client) => match client.pending.as_ref() {
                Some(pending) => (
                    pending.server_index,
                    self.configs[pending.server_index].client_max_body_size,
                ),
                None => return,
            },
            None => return,
        };

        let body_result = {
            let Some(client) = self.clients.get_mut(&fd) else {
                return;
            };
            let Some(pending) = client.pending.as_mut() else {
                return;
            };
            try_parse_request_body(
                &client.read_buf,
                &pending.head,
                MAX_HARD_REQUEST,
                body_limit,
                &mut pending.chunk_progress,
            )
        };

        let (body, consumed) = match body_result {
            BodyParseResult::NeedMore => return,
            BodyParseResult::Error(message) => {
                if let Some(client) = self.clients.get_mut(&fd) {
                    client.pending = None;
                }
                let server = self.configs[server_index].clone();
                let response = self.error_response(&server, 400, &message);
                self.queue_response(fd, response, false);
                return;
            }
            BodyParseResult::TooLarge(message) => {
                if let Some(client) = self.clients.get_mut(&fd) {
                    client.pending = None;
                }
                let server = self.configs[server_index].clone();
                let response = self.error_response(&server, 413, &message);
                self.queue_response(fd, response, false);
                return;
            }
            BodyParseResult::Complete { body, consumed } => (body, consumed),
        };

        let buffered_len = self
            .clients
            .get(&fd)
            .map(|client| client.read_buf.len())
            .unwrap_or(0);
        if consumed > buffered_len {
            self.remove_client(fd);
            return;
        }
        let mut request = {
            let Some(client) = self.clients.get_mut(&fd) else {
                return;
            };
            let Some(pending) = client.pending.take() else {
                return;
            };
            client.read_buf.drain(..consumed);
            pending.head.into_request(body)
        };

        let server = self.configs[server_index].clone();
        let normalized = match normalize_url_path(&request.path) {
            Ok(path) => path,
            Err(err) => {
                let response = self.error_response(&server, 400, &err);
                self.queue_response(fd, response, false);
                return;
            }
        };
        request.path = normalized.clone();

        let route = match server.route_for(&normalized).cloned() {
            Some(route) => route,
            None => {
                let response = self.error_response(&server, 404, "no matching route");
                self.queue_response(fd, response, !request.wants_close());
                return;
            }
        };

        match self.build_action(server_index, &server, &route, request, &normalized) {
            RequestAction::Response(response, keep_alive) => {
                self.queue_response(fd, response, keep_alive)
            }
            RequestAction::Cgi {
                spec,
                request,
                keep_alive,
                server_index,
            } => self.start_cgi(fd, server_index, spec, request, keep_alive),
        }
    }

    fn build_action(
        &mut self,
        server_index: usize,
        server: &ServerConfig,
        route: &RouteConfig,
        request: Request,
        normalized_path: &str,
    ) -> RequestAction {
        let keep_alive = !request.wants_close();
        if !route.methods.iter().any(|method| method == &request.method) {
            let mut response =
                self.error_response(server, 405, "method is not allowed for this route");
            response
                .headers
                .push(("Allow".into(), route.methods.join(", ")));
            return RequestAction::Response(response, keep_alive);
        }

        if let Some((status, target)) = &route.redirect {
            let response =
                Response::new(*status, Vec::<u8>::new()).header("Location", target.clone());
            return RequestAction::Response(response, keep_alive);
        }

        if let Some(spec) = self.find_cgi(route, normalized_path) {
            if !spec.script.is_file() {
                return RequestAction::Response(
                    self.error_response(server, 404, "CGI script not found"),
                    keep_alive,
                );
            }
            return RequestAction::Cgi {
                spec,
                request: Box::new(request),
                keep_alive,
                server_index,
            };
        }

        if normalized_path == "/api/echo" && request.method == "POST" {
            let content_type = request
                .header("content-type")
                .unwrap_or("application/octet-stream")
                .to_string();
            let response = Response::new(200, request.body).header("Content-Type", content_type);
            return RequestAction::Response(response, keep_alive);
        }

        if normalized_path == "/session" && request.method == "GET" {
            let response = self.session_response(&request);
            return RequestAction::Response(response, keep_alive);
        }

        if route.path == "/uploads" {
            match request.method.as_str() {
                "POST" => {
                    let response = self.handle_upload(server, route, &request, normalized_path);
                    return RequestAction::Response(response, keep_alive);
                }
                "DELETE" => {
                    let response = self.handle_delete(server, route, normalized_path);
                    return RequestAction::Response(response, keep_alive);
                }
                _ => {}
            }
        }

        match request.method.as_str() {
            "GET" => RequestAction::Response(
                self.handle_get(server, route, normalized_path),
                keep_alive,
            ),
            "DELETE" => RequestAction::Response(
                self.handle_delete(server, route, normalized_path),
                keep_alive,
            ),
            _ => RequestAction::Response(
                self.error_response(server, 405, "unsupported method"),
                keep_alive,
            ),
        }
    }

    fn handle_get(&self, server: &ServerConfig, route: &RouteConfig, path: &str) -> Response {
        let relative = route_relative(route, path);
        let target = match safe_join(&route.root, relative) {
            Ok(path) => path,
            Err(err) => return self.error_response(server, 403, &err),
        };
        if target.is_file() {
            return match fs::read(&target) {
                Ok(body) => Response::new(200, body).header("Content-Type", mime_type(&target)),
                Err(_) => self.error_response(server, 403, "file cannot be read"),
            };
        }
        if target.is_dir() {
            if let Some(index) = &route.index {
                let index_path = target.join(index);
                if index_path.is_file() {
                    return match fs::read(&index_path) {
                        Ok(body) => {
                            Response::new(200, body).header("Content-Type", mime_type(&index_path))
                        }
                        Err(_) => self.error_response(server, 403, "index file cannot be read"),
                    };
                }
            }
            if route.autoindex {
                return match directory_listing(&target, path) {
                    Ok(html) => Response::new(200, html)
                        .header("Content-Type", "text/html; charset=utf-8"),
                    Err(_) => self.error_response(server, 403, "directory cannot be listed"),
                };
            }
            return self.error_response(server, 403, "directory listing is disabled");
        }
        self.error_response(server, 404, "resource not found")
    }

    fn handle_upload(
        &self,
        server: &ServerConfig,
        route: &RouteConfig,
        request: &Request,
        path: &str,
    ) -> Response {
        if let Err(err) = fs::create_dir_all(&route.root) {
            return self.error_response(
                server,
                500,
                &format!("cannot create upload directory: {err}"),
            );
        }

        let mut filename = path
            .strip_prefix(&route.path)
            .unwrap_or("")
            .trim_matches('/')
            .to_string();
        let mut content = request.body.clone();

        if request
            .header("content-type")
            .map(|value| value.to_ascii_lowercase().starts_with("multipart/form-data"))
            .unwrap_or(false)
        {
            match parse_multipart_file(request) {
                Ok((name, body)) => {
                    filename = name;
                    content = body;
                }
                Err(err) => return self.error_response(server, 400, &err),
            }
        } else if filename.is_empty() {
            filename = request
                .header("x-filename")
                .unwrap_or("payload.bin")
                .to_string();
        }

        let filename = match sanitize_filename(&filename) {
            Some(name) => name,
            None => return self.error_response(server, 400, "invalid upload filename"),
        };
        let target = route.root.join(&filename);
        if let Err(err) = fs::write(&target, &content) {
            return self.error_response(server, 500, &format!("cannot store upload: {err}"));
        }
        Response::new(201, format!("uploaded {filename}\n"))
            .header("Content-Type", "text/plain; charset=utf-8")
            .header(
                "Location",
                format!("{}/{}", route.path.trim_end_matches('/'), filename),
            )
    }

    fn handle_delete(&self, server: &ServerConfig, route: &RouteConfig, path: &str) -> Response {
        let relative = route_relative(route, path);
        if relative.is_empty() {
            return self.error_response(server, 403, "refusing to delete route root");
        }
        let target = match safe_join(&route.root, relative) {
            Ok(path) => path,
            Err(err) => return self.error_response(server, 403, &err),
        };
        if !target.exists() {
            return self.error_response(server, 404, "resource not found");
        }
        if !target.is_file() {
            return self.error_response(server, 403, "only files may be deleted");
        }
        match fs::remove_file(target) {
            Ok(_) => Response::new(204, Vec::<u8>::new()),
            Err(_) => self.error_response(server, 403, "resource cannot be deleted"),
        }
    }

    fn session_response(&mut self, request: &Request) -> Response {
        let existing = request
            .header("cookie")
            .and_then(|cookie| cookie_value(cookie, "session_id"));
        let session_id = existing.unwrap_or_else(|| self.new_session_id());
        let username = query_value(&request.query, "user").unwrap_or_else(|| "auditor".into());
        let entry = self.sessions.entry(session_id.clone()).or_insert(Session {
            username: username.clone(),
            visits: 0,
            last_seen: Instant::now(),
        });
        if !username.is_empty() {
            entry.username = username;
        }
        entry.visits += 1;
        entry.last_seen = Instant::now();
        let body = format!(
            "{{\"session_id\":\"{}\",\"user\":\"{}\",\"visits\":{}}}",
            json_escape(&session_id),
            json_escape(&entry.username),
            entry.visits
        );
        Response::new(200, body)
            .header("Content-Type", "application/json; charset=utf-8")
            .header(
                "Set-Cookie",
                format!("session_id={session_id}; Path=/; Max-Age=3600; SameSite=Lax"),
            )
    }

    fn new_session_id(&mut self) -> String {
        self.session_seq = self.session_seq.wrapping_add(1);
        format!(
            "{:x}{:x}{:x}",
            std::process::id(),
            crate::util::now_millis(),
            self.session_seq
        )
    }

    fn find_cgi(&self, route: &RouteConfig, path: &str) -> Option<CgiSpec> {
        if route.cgi.is_empty() {
            return None;
        }
        let relative = route_relative(route, path);
        let segments: Vec<&str> = relative.split('/').filter(|segment| !segment.is_empty()).collect();
        let mut script_parts = Vec::new();
        for (index, segment) in segments.iter().enumerate() {
            script_parts.push(*segment);
            let lower = segment.to_ascii_lowercase();
            if let Some(mapping) = route
                .cgi
                .iter()
                .find(|mapping| lower.ends_with(&mapping.extension))
            {
                let script_rel = script_parts.join("/");
                let script = safe_join(&route.root, &script_rel).ok()?;
                let path_info = if index + 1 < segments.len() {
                    format!("/{}", segments[index + 1..].join("/"))
                } else {
                    String::new()
                };
                return Some(CgiSpec {
                    interpreter: mapping.interpreter.clone(),
                    script,
                    path_info,
                });
            }
        }
        None
    }

    fn start_cgi(
        &mut self,
        fd: RawFd,
        server_index: usize,
        spec: CgiSpec,
        request: Box<Request>,
        keep_alive: bool,
    ) {
        let port = self
            .clients
            .get(&fd)
            .and_then(|client| self.listeners.get(&client.listener_fd))
            .map(|listener| listener.addr.port())
            .unwrap_or(0);
        let launch = CgiLaunch {
            client_fd: fd,
            server_index,
            spec: &spec,
            request: &request,
            server_port: port,
            keep_alive,
        };
        match spawn_cgi(launch) {
            Ok(mut task) => {
                let io_fd = task.io_fd();
                let io_generation = self.allocate_generation();
                task.io_generation = io_generation;
                let mut interests = (libc::EPOLLIN | libc::EPOLLRDHUP) as u32;
                if task.wants_write() {
                    interests |= libc::EPOLLOUT as u32;
                }
                if self
                    .epoll_ctl_with_token(
                        libc::EPOLL_CTL_ADD,
                        io_fd,
                        interests,
                        event_token(io_fd, io_generation),
                    )
                    .is_err()
                    || self.modify_interest(fd, libc::EPOLLRDHUP as u32).is_err()
                {
                    let _ = task.child.kill();
                    let _ = task.child.wait();
                    unsafe {
                        libc::epoll_ctl(
                            self.epoll_fd,
                            libc::EPOLL_CTL_DEL,
                            io_fd,
                            std::ptr::null_mut(),
                        );
                    }
                    self.remove_client(fd);
                    return;
                }
                if let Some(client) = self.clients.get_mut(&fd) {
                    client.last_active = Instant::now();
                }
                self.cgi_io.insert(
                    io_fd,
                    CgiIoEntry {
                        client_fd: fd,
                        generation: io_generation,
                    },
                );
                self.cgi_tasks.insert(fd, task);
            }
            Err(err) => {
                let server = self.configs[server_index].clone();
                let response =
                    self.error_response(&server, 500, &format!("cannot start CGI: {err}"));
                self.queue_response(fd, response, keep_alive);
            }
        }
    }

    fn handle_cgi_io(&mut self, io_fd: RawFd, flags: u32) {
        let client_fd = match self.cgi_io.get(&io_fd) {
            Some(entry) => entry.client_fd,
            None => return,
        };
        if flags & libc::EPOLLERR as u32 != 0 {
            self.finish_cgi(client_fd, CgiFinish::Failed("CGI pipe error".into()));
            return;
        }

        let wants_write = self
            .cgi_tasks
            .get(&client_fd)
            .map(CgiTask::wants_write)
            .unwrap_or(false);
        let can_read = flags & (libc::EPOLLIN | libc::EPOLLRDHUP | libc::EPOLLHUP) as u32 != 0;
        let can_write = wants_write && flags & libc::EPOLLOUT as u32 != 0;
        let operation = match (can_read, can_write) {
            (true, true) => CgiIoOperation::ReadWrite,
            (true, false) => CgiIoOperation::Read,
            (false, true) => CgiIoOperation::Write,
            (false, false) => return,
        };

        let result = match self.cgi_tasks.get_mut(&client_fd) {
            Some(task) => match operation {
                CgiIoOperation::Read => task.read_once(),
                CgiIoOperation::Write => task.write_once(),
                CgiIoOperation::ReadWrite => task.read_once().and_then(|_| task.write_once()),
            },
            None => return,
        };
        if let Err(err) = result {
            self.finish_cgi(
                client_fd,
                CgiFinish::Failed(format!("CGI stream I/O failed: {err}")),
            );
            return;
        }

        if self
            .cgi_tasks
            .get(&client_fd)
            .map(|task| task.output.len() > MAX_CGI_OUTPUT)
            .unwrap_or(false)
        {
            self.finish_cgi(client_fd, CgiFinish::Failed("CGI output too large".into()));
            return;
        }

        let interests = self.cgi_tasks.get(&client_fd).map(|task| {
            let mut value = (libc::EPOLLIN | libc::EPOLLRDHUP) as u32;
            if task.wants_write() {
                value |= libc::EPOLLOUT as u32;
            }
            value
        });
        if let Some(interests) = interests {
            if self
                .epoll_ctl_with_token(
                    libc::EPOLL_CTL_MOD,
                    io_fd,
                    interests,
                    event_token(
                        io_fd,
                        self.cgi_tasks[&client_fd].io_generation,
                    ),
                )
                .is_err()
            {
                self.finish_cgi(client_fd, CgiFinish::Failed("cannot update CGI epoll interest".into()));
                return;
            }
        }

        let ready = self
            .cgi_tasks
            .get(&client_fd)
            .map(|task| task.io_eof && task.process_exited)
            .unwrap_or(false);
        if ready {
            self.finish_cgi(client_fd, CgiFinish::Success);
        }
    }

    fn poll_cgi_tasks(&mut self) {
        let fds: Vec<RawFd> = self.cgi_tasks.keys().copied().collect();
        for fd in fds {
            let outcome = {
                let Some(task) = self.cgi_tasks.get_mut(&fd) else {
                    continue;
                };
                if task.timed_out() {
                    let _ = task.child.kill();
                    let _ = task.child.wait();
                    Some(CgiFinish::Timeout)
                } else if !task.process_exited {
                    match task.child.try_wait() {
                        Ok(Some(status)) if status.success() => {
                            task.process_exited = true;
                            if task.io_eof {
                                Some(CgiFinish::Success)
                            } else {
                                None
                            }
                        }
                        Ok(Some(_)) => Some(CgiFinish::Failed(
                            "CGI process exited unsuccessfully".into(),
                        )),
                        Ok(None) => None,
                        Err(err) => Some(CgiFinish::Failed(format!(
                            "cannot wait for CGI process: {err}"
                        ))),
                    }
                } else if task.io_eof {
                    Some(CgiFinish::Success)
                } else {
                    None
                }
            };
            if let Some(outcome) = outcome {
                self.finish_cgi(fd, outcome);
            }
        }
    }

    fn finish_cgi(&mut self, client_fd: RawFd, outcome: CgiFinish) {
        let mut task = match self.cgi_tasks.remove(&client_fd) {
            Some(task) => task,
            None => return,
        };
        let io_fd = task.io_fd();
        self.cgi_io.remove(&io_fd);
        unsafe {
            libc::epoll_ctl(
                self.epoll_fd,
                libc::EPOLL_CTL_DEL,
                io_fd,
                std::ptr::null_mut(),
            );
        }
        if !task.process_exited {
            let _ = task.child.kill();
            let _ = task.child.wait();
        }
        if !self.clients.contains_key(&client_fd) {
            return;
        }

        let server = self.configs[task.server_index].clone();
        let response = match outcome {
            CgiFinish::Success => match parse_cgi_output(&task.output) {
                Ok(response) => response,
                Err(err) => self.error_response(&server, 502, &err),
            },
            CgiFinish::Timeout => self.error_response(&server, 504, "CGI execution timed out"),
            CgiFinish::Failed(message) => self.error_response(&server, 502, &message),
        };
        self.queue_response(client_fd, response, task.keep_alive);
    }

    fn error_response(&self, server: &ServerConfig, status: u16, message: &str) -> Response {
        if let Some(page) = server.error_pages.get(&status) {
            if let Some(root) = server.default_root() {
                let rel = page.trim_start_matches('/');
                if let Ok(path) = safe_join(root, rel) {
                    if let Ok(body) = fs::read(&path) {
                        return Response::new(status, body).header("Content-Type", mime_type(&path));
                    }
                }
            }
        }
        Response::new(
            status,
            format!(
                "<!doctype html><html><head><meta charset=\"utf-8\"><title>{status}</title></head><body><h1>{status}</h1><p>{}</p></body></html>",
                html_escape(message)
            ),
        )
        .header("Content-Type", "text/html; charset=utf-8")
    }

    fn queue_response(&mut self, fd: RawFd, response: Response, keep_alive: bool) {
        let effective_keep_alive = match self.clients.get(&fd) {
            Some(client) => keep_alive && (!client.peer_closed || !client.read_buf.is_empty()),
            None => return,
        };
        if let Some(client) = self.clients.get_mut(&fd) {
            client.write_buf = response.to_bytes(effective_keep_alive);
            client.write_pos = 0;
            client.close_after_write = !effective_keep_alive;
            client.last_active = Instant::now();
        } else {
            return;
        }
        if self
            .modify_interest(fd, (libc::EPOLLOUT | libc::EPOLLRDHUP) as u32)
            .is_err()
        {
            self.remove_client(fd);
        }
    }

    fn resolve_server_for_request(&self, fd: RawFd, host_header: &str) -> Option<usize> {
        let client = self.clients.get(&fd)?;
        let listener = self.listeners.get(&client.listener_fd)?;
        let host = host_without_port(host_header).to_ascii_lowercase();
        for index in &listener.server_indices {
            let config = self.configs.get(*index)?;
            if config
                .server_names
                .iter()
                .any(|name| name.eq_ignore_ascii_case(&host))
            {
                return Some(*index);
            }
        }
        listener.server_indices.first().copied()
    }

    fn default_server_for_client(&self, fd: RawFd) -> Option<usize> {
        let client = self.clients.get(&fd)?;
        self.listeners
            .get(&client.listener_fd)?
            .server_indices
            .first()
            .copied()
    }

    fn expire_clients(&mut self) {
        let expired: Vec<RawFd> = self
            .clients
            .iter()
            .filter(|(fd, client)| {
                !self.cgi_tasks.contains_key(fd) && client.last_active.elapsed() >= CLIENT_TIMEOUT
            })
            .map(|(fd, _)| *fd)
            .collect();
        for fd in expired {
            self.remove_client(fd);
        }
    }

    fn expire_sessions(&mut self) {
        self.sessions
            .retain(|_, session| session.last_seen.elapsed() < Duration::from_secs(3600));
    }

    fn remove_client(&mut self, fd: RawFd) {
        if let Some(mut task) = self.cgi_tasks.remove(&fd) {
            let io_fd = task.io_fd();
            self.cgi_io.remove(&io_fd);
            unsafe {
                libc::epoll_ctl(
                    self.epoll_fd,
                    libc::EPOLL_CTL_DEL,
                    io_fd,
                    std::ptr::null_mut(),
                );
            }
            let _ = task.child.kill();
            let _ = task.child.wait();
        }
        unsafe {
            libc::epoll_ctl(
                self.epoll_fd,
                libc::EPOLL_CTL_DEL,
                fd,
                std::ptr::null_mut(),
            );
        }
        self.clients.remove(&fd);
    }

    fn allocate_generation(&mut self) -> u32 {
        loop {
            self.next_generation = self.next_generation.wrapping_add(1);
            if self.next_generation != 0 {
                return self.next_generation;
            }
        }
    }

    fn epoll_add_listener(&self, fd: RawFd, events: u32) -> io::Result<()> {
        self.epoll_ctl_with_token(libc::EPOLL_CTL_ADD, fd, events, event_token(fd, 0))
    }

    fn epoll_add_client(&self, fd: RawFd, generation: u32, events: u32) -> io::Result<()> {
        self.epoll_ctl_with_token(
            libc::EPOLL_CTL_ADD,
            fd,
            events,
            event_token(fd, generation),
        )
    }

    fn modify_interest(&self, fd: RawFd, events: u32) -> io::Result<()> {
        let generation = self
            .clients
            .get(&fd)
            .map(|client| client.generation)
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "client no longer exists"))?;
        self.epoll_ctl_with_token(
            libc::EPOLL_CTL_MOD,
            fd,
            events,
            event_token(fd, generation),
        )
    }

    fn epoll_ctl_with_token(
        &self,
        operation: i32,
        fd: RawFd,
        events: u32,
        token: u64,
    ) -> io::Result<()> {
        let mut event = libc::epoll_event { events, u64: token };
        let rc = unsafe { libc::epoll_ctl(self.epoll_fd, operation, fd, &mut event) };
        if rc < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(())
        }
    }
}

#[derive(Debug)]
enum CgiFinish {
    Success,
    Timeout,
    Failed(String),
}

enum CgiIoOperation {
    Read,
    Write,
    ReadWrite,
}

impl Drop for HttpServer {
    fn drop(&mut self) {
        let fds: Vec<RawFd> = self.clients.keys().copied().collect();
        for fd in fds {
            self.remove_client(fd);
        }
        unsafe {
            libc::close(self.epoll_fd);
        }
    }
}

fn event_token(fd: RawFd, generation: u32) -> u64 {
    ((generation as u64) << 32) | (fd as u32 as u64)
}

fn token_fd(token: u64) -> RawFd {
    token as u32 as RawFd
}

fn token_generation(token: u64) -> u32 {
    (token >> 32) as u32
}

fn route_relative<'a>(route: &RouteConfig, path: &'a str) -> &'a str {
    if route.path == "/" {
        return path.trim_start_matches('/');
    }
    path.strip_prefix(&route.path)
        .unwrap_or("")
        .trim_start_matches('/')
}

fn directory_listing(dir: &Path, request_path: &str) -> io::Result<String> {
    let mut entries = fs::read_dir(dir)?.collect::<Result<Vec<_>, _>>()?;
    entries.sort_by_key(|entry| entry.file_name());
    let base = if request_path.ends_with('/') {
        request_path.to_string()
    } else {
        format!("{request_path}/")
    };
    let mut html = String::from(
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>Index</title></head><body><h1>Index</h1><ul>",
    );
    for entry in entries {
        let name = entry.file_name().to_string_lossy().to_string();
        if name.starts_with('.') {
            continue;
        }
        let display = html_escape(&name);
        let suffix = if entry.file_type()?.is_dir() { "/" } else { "" };
        html.push_str(&format!(
            "<li><a href=\"{}{}{}\">{}{}</a></li>",
            base,
            url_path_escape(&name),
            suffix,
            display,
            suffix
        ));
    }
    html.push_str("</ul></body></html>");
    Ok(html)
}

fn url_path_escape(input: &str) -> String {
    let mut out = String::new();
    for byte in input.bytes() {
        if byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.' | b'~') {
            out.push(byte as char);
        } else {
            out.push_str(&format!("%{byte:02X}"));
        }
    }
    out
}

fn parse_multipart_file(request: &Request) -> Result<(String, Vec<u8>), String> {
    let content_type = request
        .header("content-type")
        .ok_or_else(|| "multipart request has no Content-Type".to_string())?;
    let boundary = content_type
        .split(';')
        .map(str::trim)
        .find_map(|part| part.strip_prefix("boundary="))
        .map(|value| value.trim_matches('"'))
        .filter(|value| !value.is_empty())
        .ok_or_else(|| "multipart boundary is missing".to_string())?;
    let marker = format!("--{boundary}").into_bytes();
    let mut pos = 0usize;
    while let Some(start_rel) = find_bytes(&request.body[pos..], &marker) {
        let start = pos + start_rel + marker.len();
        if request.body.get(start..start + 2) == Some(b"--") {
            break;
        }
        let part_start = if request.body.get(start..start + 2) == Some(b"\r\n") {
            start + 2
        } else {
            return Err("malformed multipart boundary".into());
        };
        let headers_end_rel = find_bytes(&request.body[part_start..], b"\r\n\r\n")
            .ok_or_else(|| "malformed multipart headers".to_string())?;
        let headers_end = part_start + headers_end_rel;
        let headers = String::from_utf8_lossy(&request.body[part_start..headers_end]);
        let data_start = headers_end + 4;
        let next_marker = format!("\r\n--{boundary}").into_bytes();
        let data_end_rel = find_bytes(&request.body[data_start..], &next_marker)
            .ok_or_else(|| "unterminated multipart part".to_string())?;
        let data_end = data_start + data_end_rel;
        if let Some(filename) = multipart_filename(&headers) {
            let safe = sanitize_filename(&filename)
                .ok_or_else(|| "invalid multipart filename".to_string())?;
            return Ok((safe, request.body[data_start..data_end].to_vec()));
        }
        pos = data_end + 2;
    }
    Err("multipart body contains no file part".into())
}

fn multipart_filename(headers: &str) -> Option<String> {
    for line in headers.lines() {
        if !line
            .to_ascii_lowercase()
            .starts_with("content-disposition:")
        {
            continue;
        }
        for part in line.split(';').map(str::trim) {
            if let Some(value) = part.strip_prefix("filename=") {
                return Some(value.trim_matches('"').to_string());
            }
        }
    }
    None
}

fn find_bytes(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() {
        return Some(0);
    }
    haystack
        .windows(needle.len())
        .position(|window| window == needle)
}

fn cookie_value(header: &str, key: &str) -> Option<String> {
    header.split(';').find_map(|pair| {
        let (name, value) = pair.trim().split_once('=')?;
        if name == key {
            Some(value.to_string())
        } else {
            None
        }
    })
}

fn query_value(query: &str, key: &str) -> Option<String> {
    query.split('&').find_map(|pair| {
        let (name, value) = pair.split_once('=').unwrap_or((pair, ""));
        if name != key {
            return None;
        }
        let replaced = value.replace('+', " ");
        crate::util::percent_decode(&replaced).ok()
    })
}

fn host_without_port(header: &str) -> &str {
    let trimmed = header.trim();
    if trimmed.starts_with('[') {
        if let Some(end) = trimmed.find(']') {
            return &trimmed[1..end];
        }
    }
    trimmed.split(':').next().unwrap_or(trimmed)
}

fn json_escape(input: &str) -> String {
    input
        .replace('\\', "\\\\")
        .replace('"', "\\\"")
        .replace('\n', "\\n")
        .replace('\r', "\\r")
        .replace('\t', "\\t")
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;
    use std::path::PathBuf;

    fn upload_route() -> RouteConfig {
        RouteConfig {
            path: "/uploads".into(),
            methods: vec!["GET".into(), "POST".into(), "DELETE".into()],
            root: PathBuf::from("./public/uploads"),
            index: None,
            autoindex: true,
            redirect: None,
            cgi: vec![],
        }
    }

    #[test]
    fn route_relative_strips_location_prefix() {
        assert_eq!(
            route_relative(&upload_route(), "/uploads/a.txt"),
            "a.txt"
        );
    }

    #[test]
    fn event_tokens_distinguish_reused_file_descriptors() {
        let fd = 42;
        assert_ne!(event_token(fd, 1), event_token(fd, 2));
        assert_eq!(token_fd(event_token(fd, 9)), fd);
        assert_eq!(token_generation(event_token(fd, 9)), 9);
    }

    #[test]
    fn extracts_multipart_file() {
        let boundary = "abc123";
        let body = format!(
            "--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"hello.txt\"\r\nContent-Type: text/plain\r\n\r\nhello\r\n--{boundary}--\r\n"
        )
        .into_bytes();
        let mut headers = HashMap::new();
        headers.insert(
            "content-type".into(),
            format!("multipart/form-data; boundary={boundary}"),
        );
        let request = Request {
            method: "POST".into(),
            target: "/uploads".into(),
            path: "/uploads".into(),
            query: String::new(),
            version: "HTTP/1.1".into(),
            headers,
            body,
        };
        let (name, bytes) = parse_multipart_file(&request).unwrap();
        assert_eq!(name, "hello.txt");
        assert_eq!(bytes, b"hello");
    }
}
