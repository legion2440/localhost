use crate::http::{reason_phrase, Response};
use crate::util::now_millis;
use std::fs::{self, File};
use std::io;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

#[derive(Debug, Clone)]
pub struct CgiSpec {
    pub interpreter: PathBuf,
    pub script: PathBuf,
    pub path_info: String,
}

pub struct CgiTask {
    pub client_fd: i32,
    pub server_index: usize,
    pub child: Child,
    pub output_path: PathBuf,
    pub input_path: PathBuf,
    pub started: Instant,
    pub timeout: Duration,
    pub keep_alive: bool,
}

impl CgiTask {
    pub fn timed_out(&self) -> bool {
        self.started.elapsed() >= self.timeout
    }

    pub fn cleanup_files(&self) {
        let _ = fs::remove_file(&self.input_path);
        let _ = fs::remove_file(&self.output_path);
    }
}

pub fn spawn_cgi(
    client_fd: i32,
    server_index: usize,
    spec: &CgiSpec,
    method: &str,
    query: &str,
    content_type: Option<&str>,
    cookie: Option<&str>,
    host: &str,
    server_port: u16,
    body: &[u8],
    keep_alive: bool,
) -> io::Result<CgiTask> {
    let nonce = now_millis();
    let pid = std::process::id();
    let tmp = std::env::temp_dir();
    let input_path = tmp.join(format!("localhost-cgi-{pid}-{client_fd}-{nonce}.in"));
    let output_path = tmp.join(format!("localhost-cgi-{pid}-{client_fd}-{nonce}.out"));

    fs::write(&input_path, body)?;
    let input = File::open(&input_path)?;
    let output = File::create(&output_path)?;

    let parent = spec.script.parent().unwrap_or_else(|| Path::new("."));
    let script_arg = spec
        .script
        .file_name()
        .map(PathBuf::from)
        .unwrap_or_else(|| spec.script.clone());

    let mut cmd = Command::new(&spec.interpreter);
    cmd.arg(script_arg)
        .current_dir(parent)
        .stdin(Stdio::from(input))
        .stdout(Stdio::from(output))
        .stderr(Stdio::null())
        .env("GATEWAY_INTERFACE", "CGI/1.1")
        .env("SERVER_PROTOCOL", "HTTP/1.1")
        .env("SERVER_SOFTWARE", "localhost/0.1")
        .env("REQUEST_METHOD", method)
        .env("QUERY_STRING", query)
        .env("PATH_INFO", &spec.path_info)
        .env("SCRIPT_FILENAME", &spec.script)
        .env("SCRIPT_NAME", spec.script.to_string_lossy().as_ref())
        .env("CONTENT_LENGTH", body.len().to_string())
        .env("SERVER_PORT", server_port.to_string())
        .env("HTTP_HOST", host);
    if let Some(value) = content_type {
        cmd.env("CONTENT_TYPE", value);
    }
    if let Some(value) = cookie {
        cmd.env("HTTP_COOKIE", value);
    }

    let child = match cmd.spawn() {
        Ok(child) => child,
        Err(err) => {
            let _ = fs::remove_file(&input_path);
            let _ = fs::remove_file(&output_path);
            return Err(err);
        }
    };

    Ok(CgiTask {
        client_fd,
        server_index,
        child,
        output_path,
        input_path,
        started: Instant::now(),
        timeout: Duration::from_secs(5),
        keep_alive,
    })
}

pub fn parse_cgi_output(bytes: &[u8]) -> Result<Response, String> {
    let (head, body) = if let Some(pos) = find(bytes, b"\r\n\r\n") {
        (&bytes[..pos], &bytes[pos + 4..])
    } else if let Some(pos) = find(bytes, b"\n\n") {
        (&bytes[..pos], &bytes[pos + 2..])
    } else {
        return Err("CGI did not emit a header/body separator".into());
    };

    let header_text = String::from_utf8_lossy(head);
    let mut status = 200u16;
    let mut headers = Vec::new();
    for raw_line in header_text.lines() {
        let line = raw_line.trim_end_matches('\r');
        if line.trim().is_empty() {
            continue;
        }
        let (name, value) = line
            .split_once(':')
            .ok_or_else(|| format!("malformed CGI header `{line}`"))?;
        if name.eq_ignore_ascii_case("Status") {
            let code = value
                .trim()
                .split_whitespace()
                .next()
                .ok_or_else(|| "empty CGI Status header".to_string())?;
            status = code
                .parse()
                .map_err(|_| format!("invalid CGI status `{value}`"))?;
        } else {
            headers.push((name.trim().to_string(), value.trim().to_string()));
        }
    }

    if !(100..600).contains(&status) {
        return Err(format!("invalid CGI status {status}"));
    }

    let mut response = Response::new(status, body.to_vec());
    response.headers = headers;
    Ok(response)
}

pub fn cgi_error_response(status: u16, message: &str) -> Response {
    Response::new(
        status,
        format!(
            "<!doctype html><html><body><h1>{status} {}</h1><p>{}</p></body></html>",
            reason_phrase(status),
            crate::util::html_escape(message)
        ),
    )
    .header("Content-Type", "text/html; charset=utf-8")
}

fn find(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    haystack.windows(needle.len()).position(|window| window == needle)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_lf_only_cgi_headers() {
        let raw = b"Status: 201 Created\nContent-Type: text/plain\n\nhello";
        let response = parse_cgi_output(raw).unwrap();
        assert_eq!(response.status, 201);
        assert_eq!(response.body, b"hello");
    }
}
