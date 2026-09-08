use crate::http::{reason_phrase, Request, Response};
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::net::Shutdown;
use std::os::fd::{AsRawFd, FromRawFd, IntoRawFd, RawFd};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

#[derive(Debug, Clone)]
pub struct CgiSpec {
    pub interpreter: PathBuf,
    pub script: PathBuf,
    pub path_info: String,
}

pub struct CgiLaunch<'a> {
    pub client_fd: RawFd,
    pub server_index: usize,
    pub spec: &'a CgiSpec,
    pub request: &'a Request,
    pub server_port: u16,
    pub keep_alive: bool,
}

pub struct CgiTask {
    pub server_index: usize,
    pub child: Child,
    pub io: UnixStream,
    pub input: Vec<u8>,
    pub input_pos: usize,
    pub output: Vec<u8>,
    pub started: Instant,
    pub timeout: Duration,
    pub keep_alive: bool,
    pub io_generation: u32,
    pub input_closed: bool,
    pub io_eof: bool,
    pub process_exited: bool,
}

fn cgi_input_closed_error(err: &io::Error) -> bool {
    matches!(
        err.kind(),
        io::ErrorKind::BrokenPipe
            | io::ErrorKind::ConnectionReset
            | io::ErrorKind::NotConnected
    )
}

impl CgiTask {
    pub fn timed_out(&self) -> bool {
        self.started.elapsed() >= self.timeout
    }

    pub fn io_fd(&self) -> RawFd {
        self.io.as_raw_fd()
    }

    pub fn wants_write(&self) -> bool {
        !self.input_closed && self.input_pos < self.input.len()
    }

    pub fn close_input(&mut self) -> io::Result<()> {
        if !self.input_closed {
            match self.io.shutdown(Shutdown::Write) {
                Ok(()) => {}
                Err(err) if cgi_input_closed_error(&err) => {}
                Err(err) => return Err(err),
            }
            self.input_closed = true;
        }
        Ok(())
    }

    pub fn write_once(&mut self) -> io::Result<()> {
        if self.io_eof {
            self.input_closed = true;
            return Ok(());
        }
        if !self.wants_write() {
            self.close_input()?;
            return Ok(());
        }
        match self.io.write(&self.input[self.input_pos..]) {
            Ok(0) => {
                self.input_closed = true;
                Ok(())
            }
            Ok(count) => {
                self.input_pos += count;
                if self.input_pos == self.input.len() {
                    self.close_input()?;
                }
                Ok(())
            }
            Err(err) if err.kind() == io::ErrorKind::WouldBlock => Ok(()),
            Err(err) if cgi_input_closed_error(&err) => {
                self.input_closed = true;
                Ok(())
            }
            Err(err) => Err(err),
        }
    }

    pub fn read_once(&mut self) -> io::Result<()> {
        let mut buffer = [0u8; 65536];
        match self.io.read(&mut buffer) {
            Ok(0) => {
                self.io_eof = true;
                Ok(())
            }
            Ok(count) => {
                self.output.extend_from_slice(&buffer[..count]);
                Ok(())
            }
            Err(err) if err.kind() == io::ErrorKind::WouldBlock => Ok(()),
            Err(err) => Err(err),
        }
    }
}

pub fn spawn_cgi(launch: CgiLaunch<'_>) -> io::Result<CgiTask> {
    let (parent_io, child_io) = UnixStream::pair()?;
    parent_io.set_nonblocking(true)?;

    let child_stdin_socket = child_io.try_clone()?;
    let child_stdin = unsafe { File::from_raw_fd(child_stdin_socket.into_raw_fd()) };
    let child_stdout = unsafe { File::from_raw_fd(child_io.into_raw_fd()) };

    let script_path = fs::canonicalize(&launch.spec.script)
        .or_else(|_| std::env::current_dir().map(|cwd| cwd.join(&launch.spec.script)))?;
    let parent = script_path.parent().unwrap_or_else(|| Path::new("."));
    let script_arg = script_path
        .file_name()
        .map(PathBuf::from)
        .unwrap_or_else(|| script_path.clone());

    let request = launch.request;
    let mut cmd = Command::new(&launch.spec.interpreter);
    cmd.arg(script_arg)
        .current_dir(parent)
        .stdin(Stdio::from(child_stdin))
        .stdout(Stdio::from(child_stdout))
        .stderr(Stdio::null())
        .env("GATEWAY_INTERFACE", "CGI/1.1")
        .env("SERVER_PROTOCOL", &request.version)
        .env("SERVER_SOFTWARE", "localhost/0.1")
        .env("REQUEST_METHOD", &request.method)
        .env("REQUEST_URI", &request.target)
        .env("QUERY_STRING", &request.query)
        .env("PATH_INFO", &launch.spec.path_info)
        .env("SCRIPT_FILENAME", &script_path)
        .env("SCRIPT_NAME", launch.spec.script.to_string_lossy().as_ref())
        .env("CONTENT_LENGTH", request.body.len().to_string())
        .env("SERVER_PORT", launch.server_port.to_string())
        .env("HTTP_HOST", request.host());
    if let Some(value) = request.header("content-type") {
        cmd.env("CONTENT_TYPE", value);
    }
    if let Some(value) = request.header("cookie") {
        cmd.env("HTTP_COOKIE", value);
    }

    let child = cmd.spawn()?;
    let mut task = CgiTask {
        server_index: launch.server_index,
        child,
        io: parent_io,
        input: request.body.clone(),
        input_pos: 0,
        output: Vec::new(),
        started: Instant::now(),
        timeout: Duration::from_secs(5),
        keep_alive: launch.keep_alive,
        io_generation: 0,
        input_closed: false,
        io_eof: false,
        process_exited: false,
    };
    if task.input.is_empty() {
        task.close_input()?;
    }
    let _ = launch.client_fd;
    Ok(task)
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
