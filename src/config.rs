use std::collections::{HashMap, HashSet};
use std::fs;
use std::net::SocketAddr;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone)]
pub struct CgiMapping {
    pub extension: String,
    pub interpreter: PathBuf,
}

#[derive(Debug, Clone)]
pub struct RouteConfig {
    pub path: String,
    pub methods: Vec<String>,
    pub root: PathBuf,
    pub index: Option<String>,
    pub autoindex: bool,
    pub redirect: Option<(u16, String)>,
    pub cgi: Vec<CgiMapping>,
}

#[derive(Debug, Clone)]
pub struct ServerConfig {
    pub listens: Vec<SocketAddr>,
    pub server_names: Vec<String>,
    pub client_max_body_size: usize,
    pub error_pages: HashMap<u16, String>,
    pub routes: Vec<RouteConfig>,
}

impl ServerConfig {
    pub fn route_for(&self, request_path: &str) -> Option<&RouteConfig> {
        self.routes
            .iter()
            .filter(|route| route_matches(&route.path, request_path))
            .max_by_key(|route| route.path.len())
    }

    pub fn default_root(&self) -> Option<&Path> {
        self.routes
            .iter()
            .find(|route| route.path == "/")
            .map(|route| route.root.as_path())
    }
}

fn route_matches(prefix: &str, path: &str) -> bool {
    if prefix == "/" {
        return path.starts_with('/');
    }
    if path == prefix {
        return true;
    }
    path.strip_prefix(prefix)
        .map(|tail| tail.starts_with('/'))
        .unwrap_or(false)
}

#[derive(Debug, Clone)]
struct RawServer {
    lines: Vec<String>,
}

pub fn load_config(path: &Path) -> Result<Vec<ServerConfig>, String> {
    let text = fs::read_to_string(path)
        .map_err(|e| format!("cannot read configuration {}: {e}", path.display()))?;
    parse_config(&text)
}

pub fn parse_config(text: &str) -> Result<Vec<ServerConfig>, String> {
    let raw_servers = split_server_blocks(text)?;
    if raw_servers.is_empty() {
        return Err("configuration contains no server blocks".into());
    }

    let mut valid = Vec::new();
    let mut errors = Vec::new();
    for (idx, raw) in raw_servers.into_iter().enumerate() {
        match parse_server(raw) {
            Ok(server) => valid.push(server),
            Err(err) => errors.push(format!("server #{} skipped: {err}", idx + 1)),
        }
    }

    for err in &errors {
        eprintln!("config warning: {err}");
    }

    if valid.is_empty() {
        return Err(errors.join("; "));
    }
    Ok(valid)
}

fn clean_line(line: &str) -> String {
    // Comments are not required by the subject, but accepting them makes the
    // supplied audit configuration friendlier without affecting the grammar.
    line.split('#').next().unwrap_or("").trim().to_string()
}

fn split_server_blocks(text: &str) -> Result<Vec<RawServer>, String> {
    let mut blocks = Vec::new();
    let mut current = Vec::new();
    let mut depth = 0i32;
    let mut in_server = false;

    for (line_no, original) in text.lines().enumerate() {
        let line = clean_line(original);
        if line.is_empty() {
            continue;
        }

        if !in_server {
            if line == "server {" {
                in_server = true;
                depth = 1;
                current.clear();
                continue;
            }
            return Err(format!("line {}: expected `server {{`", line_no + 1));
        }

        let opens = line.chars().filter(|c| *c == '{').count() as i32;
        let closes = line.chars().filter(|c| *c == '}').count() as i32;
        depth += opens - closes;

        if depth < 0 {
            return Err(format!("line {}: unmatched closing brace", line_no + 1));
        }
        if depth == 0 {
            in_server = false;
            blocks.push(RawServer { lines: current.clone() });
            current.clear();
        } else {
            current.push(line);
        }
    }

    if in_server || depth != 0 {
        return Err("unterminated server block".into());
    }
    Ok(blocks)
}

fn parse_server(raw: RawServer) -> Result<ServerConfig, String> {
    let mut listens = Vec::new();
    let mut seen_listens = HashSet::new();
    let mut server_names = Vec::new();
    let mut client_max_body_size = 1024 * 1024;
    let mut error_pages = HashMap::new();
    let mut routes = Vec::new();

    let mut i = 0usize;
    while i < raw.lines.len() {
        let line = &raw.lines[i];
        if line.starts_with("location ") && line.ends_with('{') {
            let path = line
                .trim_end_matches('{')
                .trim()
                .split_whitespace()
                .nth(1)
                .ok_or_else(|| "invalid location declaration".to_string())?;
            if !path.starts_with('/') {
                return Err(format!("location path must start with '/': {path}"));
            }
            let mut nested = Vec::new();
            i += 1;
            let mut depth = 1i32;
            while i < raw.lines.len() && depth > 0 {
                let nested_line = &raw.lines[i];
                let opens = nested_line.chars().filter(|c| *c == '{').count() as i32;
                let closes = nested_line.chars().filter(|c| *c == '}').count() as i32;
                depth += opens - closes;
                if depth > 0 {
                    nested.push(nested_line.clone());
                }
                i += 1;
            }
            if depth != 0 {
                return Err(format!("unterminated location {path}"));
            }
            routes.push(parse_route(path, &nested)?);
            continue;
        }

        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.is_empty() {
            i += 1;
            continue;
        }
        match parts[0] {
            "listen" => {
                if parts.len() != 2 {
                    return Err(format!("invalid listen directive: {line}"));
                }
                let addr: SocketAddr = parts[1]
                    .parse()
                    .map_err(|_| format!("invalid listen address: {}", parts[1]))?;
                if !seen_listens.insert(addr) {
                    return Err(format!("duplicate listen directive in one server: {addr}"));
                }
                listens.push(addr);
            }
            "server_name" => {
                if parts.len() < 2 {
                    return Err("server_name needs at least one name".into());
                }
                server_names.extend(parts[1..].iter().map(|s| s.to_ascii_lowercase()));
            }
            "client_max_body_size" => {
                if parts.len() != 2 {
                    return Err(format!("invalid client_max_body_size: {line}"));
                }
                client_max_body_size = parse_size(parts[1])?;
            }
            "error_page" => {
                if parts.len() != 3 {
                    return Err(format!("invalid error_page directive: {line}"));
                }
                let code: u16 = parts[1]
                    .parse()
                    .map_err(|_| format!("invalid error status: {}", parts[1]))?;
                error_pages.insert(code, parts[2].to_string());
            }
            "}" => {}
            other => return Err(format!("unknown server directive `{other}`")),
        }
        i += 1;
    }

    if listens.is_empty() {
        return Err("server has no listen directive".into());
    }
    if routes.is_empty() {
        return Err("server has no locations".into());
    }
    if !routes.iter().any(|r| r.path == "/") {
        return Err("server needs a `/` location".into());
    }

    Ok(ServerConfig {
        listens,
        server_names,
        client_max_body_size,
        error_pages,
        routes,
    })
}

fn parse_route(path: &str, lines: &[String]) -> Result<RouteConfig, String> {
    let mut methods = vec!["GET".to_string()];
    let mut root = PathBuf::from(".");
    let mut index = None;
    let mut autoindex = false;
    let mut redirect = None;
    let mut cgi = Vec::new();

    for line in lines {
        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.is_empty() {
            continue;
        }
        match parts[0] {
            "methods" => {
                if parts.len() < 2 {
                    return Err(format!("location {path}: methods list is empty"));
                }
                methods = parts[1..].iter().map(|m| m.to_ascii_uppercase()).collect();
            }
            "root" => {
                if parts.len() != 2 {
                    return Err(format!("location {path}: invalid root"));
                }
                root = PathBuf::from(parts[1]);
            }
            "index" => {
                if parts.len() != 2 {
                    return Err(format!("location {path}: invalid index"));
                }
                index = Some(parts[1].to_string());
            }
            "autoindex" => {
                if parts.len() != 2 || !matches!(parts[1], "on" | "off") {
                    return Err(format!("location {path}: autoindex must be on/off"));
                }
                autoindex = parts[1] == "on";
            }
            "return" => {
                if parts.len() != 3 {
                    return Err(format!("location {path}: invalid return directive"));
                }
                let status: u16 = parts[1]
                    .parse()
                    .map_err(|_| format!("location {path}: invalid redirect status"))?;
                if !(300..400).contains(&status) {
                    return Err(format!("location {path}: redirect status must be 3xx"));
                }
                redirect = Some((status, parts[2].to_string()));
            }
            "cgi_extension" => {
                if parts.len() != 3 || !parts[1].starts_with('.') {
                    return Err(format!("location {path}: invalid cgi_extension"));
                }
                cgi.push(CgiMapping {
                    extension: parts[1].to_ascii_lowercase(),
                    interpreter: PathBuf::from(parts[2]),
                });
            }
            other => return Err(format!("location {path}: unknown directive `{other}`")),
        }
    }

    Ok(RouteConfig {
        path: path.to_string(),
        methods,
        root,
        index,
        autoindex,
        redirect,
        cgi,
    })
}

fn parse_size(value: &str) -> Result<usize, String> {
    let lower = value.to_ascii_lowercase();
    let (number, multiplier) = if let Some(v) = lower.strip_suffix('k') {
        (v, 1024usize)
    } else if let Some(v) = lower.strip_suffix('m') {
        (v, 1024usize * 1024)
    } else {
        (lower.as_str(), 1usize)
    };
    let base: usize = number
        .parse()
        .map_err(|_| format!("invalid size `{value}`"))?;
    base.checked_mul(multiplier)
        .ok_or_else(|| format!("size `{value}` is too large"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_virtual_host_config() {
        let cfg = r#"
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
            location /cgi-bin {
                methods GET POST
                root ./cgi-bin
                cgi_extension .py /usr/bin/python3
            }
        }
        "#;
        let servers = parse_config(cfg).unwrap();
        assert_eq!(servers.len(), 1);
        assert_eq!(servers[0].client_max_body_size, 2 * 1024 * 1024);
        assert_eq!(servers[0].route_for("/cgi-bin/test.py").unwrap().path, "/cgi-bin");
    }

    #[test]
    fn rejects_duplicate_listen_inside_same_server() {
        let cfg = r#"
        server {
            listen 127.0.0.1:8080
            listen 127.0.0.1:8080
            location / {
                root ./public
            }
        }
        "#;
        assert!(parse_config(cfg).is_err());
    }

    #[test]
    fn accepts_same_listener_for_virtual_servers() {
        let cfg = r#"
        server {
            listen 127.0.0.1:8080
            server_name a.test
            location / {
                root ./a
            }
        }
        server {
            listen 127.0.0.1:8080
            server_name b.test
            location / {
                root ./b
            }
        }
        "#;
        assert_eq!(parse_config(cfg).unwrap().len(), 2);
    }
}
