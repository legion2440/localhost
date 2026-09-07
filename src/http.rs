use std::collections::HashMap;
use std::path::Path;

#[derive(Debug, Clone)]
pub struct Request {
    pub method: String,
    pub target: String,
    pub path: String,
    pub query: String,
    pub version: String,
    pub headers: HashMap<String, String>,
    pub body: Vec<u8>,
}

impl Request {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.get(&name.to_ascii_lowercase()).map(String::as_str)
    }

    pub fn host(&self) -> &str {
        self.header("host").unwrap_or("")
    }

    pub fn wants_close(&self) -> bool {
        if self.version != "HTTP/1.1" || self.target.is_empty() {
            return true;
        }
        self.header("connection")
            .map(|v| v.eq_ignore_ascii_case("close"))
            .unwrap_or(false)
    }
}

#[derive(Debug)]
pub enum ParseResult {
    NeedMore,
    Complete { request: Request, consumed: usize },
    Error(String),
}

pub fn try_parse_request(buffer: &[u8], hard_limit: usize) -> ParseResult {
    if buffer.len() > hard_limit {
        return ParseResult::Error("request exceeds hard server limit".into());
    }
    let header_end = match find_bytes(buffer, b"\r\n\r\n") {
        Some(pos) => pos,
        None => return ParseResult::NeedMore,
    };
    let header_bytes = &buffer[..header_end];
    let header_text = match std::str::from_utf8(header_bytes) {
        Ok(v) => v,
        Err(_) => return ParseResult::Error("headers are not valid UTF-8".into()),
    };
    let mut lines = header_text.split("\r\n");
    let request_line = match lines.next() {
        Some(v) => v,
        None => return ParseResult::Error("missing request line".into()),
    };
    let request_parts: Vec<&str> = request_line.split_whitespace().collect();
    if request_parts.len() != 3 {
        return ParseResult::Error("invalid request line".into());
    }
    let method = request_parts[0].to_ascii_uppercase();
    if method.is_empty() || !method.bytes().all(|b| b.is_ascii_uppercase() || b == b'-') {
        return ParseResult::Error("invalid HTTP method".into());
    }
    if request_parts[2] != "HTTP/1.1" {
        return ParseResult::Error("only HTTP/1.1 is supported".into());
    }
    let target = request_parts[1].to_string();
    let (raw_path, query) = match target.split_once('?') {
        Some((p, q)) => (p.to_string(), q.to_string()),
        None => (target.clone(), String::new()),
    };
    if !raw_path.starts_with('/') {
        return ParseResult::Error("request target must use origin-form".into());
    }

    let mut headers = HashMap::new();
    for line in lines {
        if line.is_empty() {
            continue;
        }
        let (name, value) = match line.split_once(':') {
            Some(v) => v,
            None => return ParseResult::Error("malformed header line".into()),
        };
        let name = name.trim().to_ascii_lowercase();
        if name.is_empty()
            || !name
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_'))
        {
            return ParseResult::Error("invalid header name".into());
        }
        let value = value.trim().to_string();
        if let Some(previous) = headers.get(&name) {
            if name == "content-length" && previous != &value {
                return ParseResult::Error("conflicting Content-Length headers".into());
            }
        }
        headers.insert(name, value);
    }

    if !headers.contains_key("host") {
        return ParseResult::Error("HTTP/1.1 Host header is required".into());
    }

    let body_start = header_end + 4;
    let transfer_encoding = headers
        .get("transfer-encoding")
        .map(|v| v.to_ascii_lowercase());
    let content_length = headers.get("content-length");
    if transfer_encoding.is_some() && content_length.is_some() {
        return ParseResult::Error("Content-Length and Transfer-Encoding cannot be combined".into());
    }

    let (body, consumed) = if let Some(encoding) = transfer_encoding {
        if !encoding
            .split(',')
            .map(str::trim)
            .any(|token| token.eq_ignore_ascii_case("chunked"))
        {
            return ParseResult::Error("unsupported Transfer-Encoding".into());
        }
        match decode_chunked(&buffer[body_start..], hard_limit) {
            Ok(Some((body, used))) => (body, body_start + used),
            Ok(None) => return ParseResult::NeedMore,
            Err(err) => return ParseResult::Error(err),
        }
    } else if let Some(value) = content_length {
        let len: usize = match value.parse() {
            Ok(v) => v,
            Err(_) => return ParseResult::Error("invalid Content-Length".into()),
        };
        if len > hard_limit {
            return ParseResult::Error("request body exceeds hard server limit".into());
        }
        if buffer.len() < body_start + len {
            return ParseResult::NeedMore;
        }
        (buffer[body_start..body_start + len].to_vec(), body_start + len)
    } else {
        (Vec::new(), body_start)
    };

    ParseResult::Complete {
        request: Request {
            method,
            target,
            path: raw_path,
            query,
            version: "HTTP/1.1".to_string(),
            headers,
            body,
        },
        consumed,
    }
}

fn decode_chunked(input: &[u8], hard_limit: usize) -> Result<Option<(Vec<u8>, usize)>, String> {
    let mut out = Vec::new();
    let mut pos = 0usize;
    loop {
        let line_end = match find_bytes(&input[pos..], b"\r\n") {
            Some(v) => pos + v,
            None => return Ok(None),
        };
        let line = std::str::from_utf8(&input[pos..line_end])
            .map_err(|_| "invalid chunk size line".to_string())?;
        let size_text = line.split(';').next().unwrap_or("").trim();
        if size_text.is_empty() {
            return Err("empty chunk size".into());
        }
        let size = usize::from_str_radix(size_text, 16)
            .map_err(|_| "invalid hexadecimal chunk size".to_string())?;
        pos = line_end + 2;

        if size == 0 {
            // An empty trailer section is exactly one CRLF. Check it before
            // searching for CRLFCRLF so a pipelined request cannot be mistaken
            // for trailers merely because its headers end with CRLFCRLF.
            if input.get(pos..pos + 2) == Some(b"\r\n") {
                pos += 2;
                return Ok(Some((out, pos)));
            }
            let trailer_end = match find_bytes(&input[pos..], b"\r\n\r\n") {
                Some(v) => pos + v + 4,
                None => return Ok(None),
            };
            return Ok(Some((out, trailer_end)));
        }

        if out.len().saturating_add(size) > hard_limit {
            return Err("decoded chunked body exceeds hard server limit".into());
        }
        if input.len() < pos + size + 2 {
            return Ok(None);
        }
        out.extend_from_slice(&input[pos..pos + size]);
        pos += size;
        if input.get(pos..pos + 2) != Some(b"\r\n") {
            return Err("chunk data is not followed by CRLF".into());
        }
        pos += 2;
    }
}

pub fn find_bytes(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    haystack.windows(needle.len()).position(|window| window == needle)
}

#[derive(Debug, Clone)]
pub struct Response {
    pub status: u16,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
}

impl Response {
    pub fn new(status: u16, body: impl Into<Vec<u8>>) -> Self {
        Self {
            status,
            headers: Vec::new(),
            body: body.into(),
        }
    }

    pub fn header(mut self, name: impl Into<String>, value: impl Into<String>) -> Self {
        self.headers.push((name.into(), value.into()));
        self
    }

    pub fn to_bytes(&self, keep_alive: bool) -> Vec<u8> {
        let mut out = Vec::new();
        let head = format!("HTTP/1.1 {} {}\r\n", self.status, reason_phrase(self.status));
        out.extend_from_slice(head.as_bytes());
        let mut has_length = false;
        let mut has_type = false;
        let mut has_connection = false;
        for (name, value) in &self.headers {
            if name.eq_ignore_ascii_case("content-length") {
                has_length = true;
            } else if name.eq_ignore_ascii_case("content-type") {
                has_type = true;
            } else if name.eq_ignore_ascii_case("connection") {
                has_connection = true;
            }
            out.extend_from_slice(name.as_bytes());
            out.extend_from_slice(b": ");
            out.extend_from_slice(value.as_bytes());
            out.extend_from_slice(b"\r\n");
        }
        if !has_type {
            out.extend_from_slice(b"Content-Type: text/plain; charset=utf-8\r\n");
        }
        if !has_length {
            out.extend_from_slice(format!("Content-Length: {}\r\n", self.body.len()).as_bytes());
        }
        if !has_connection {
            out.extend_from_slice(
                if keep_alive {
                    b"Connection: keep-alive\r\n".as_slice()
                } else {
                    b"Connection: close\r\n".as_slice()
                },
            );
        }
        out.extend_from_slice(b"Server: localhost/0.1\r\n\r\n");
        out.extend_from_slice(&self.body);
        out
    }
}

pub fn reason_phrase(status: u16) -> &'static str {
    match status {
        200 => "OK",
        201 => "Created",
        204 => "No Content",
        301 => "Moved Permanently",
        302 => "Found",
        303 => "See Other",
        307 => "Temporary Redirect",
        308 => "Permanent Redirect",
        400 => "Bad Request",
        403 => "Forbidden",
        404 => "Not Found",
        405 => "Method Not Allowed",
        408 => "Request Timeout",
        413 => "Payload Too Large",
        500 => "Internal Server Error",
        502 => "Bad Gateway",
        504 => "Gateway Timeout",
        _ => "Unknown",
    }
}

pub fn mime_type(path: &Path) -> &'static str {
    match path
        .extension()
        .and_then(|v| v.to_str())
        .unwrap_or("")
        .to_ascii_lowercase()
        .as_str()
    {
        "html" | "htm" => "text/html; charset=utf-8",
        "css" => "text/css; charset=utf-8",
        "js" => "application/javascript; charset=utf-8",
        "json" => "application/json; charset=utf-8",
        "txt" => "text/plain; charset=utf-8",
        "svg" => "image/svg+xml",
        "png" => "image/png",
        "jpg" | "jpeg" => "image/jpeg",
        "gif" => "image/gif",
        "webp" => "image/webp",
        "ico" => "image/x-icon",
        "pdf" => "application/pdf",
        "wasm" => "application/wasm",
        _ => "application/octet-stream",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_content_length_request() {
        let raw = b"POST /api/echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello";
        match try_parse_request(raw, 1024) {
            ParseResult::Complete { request, .. } => assert_eq!(request.body, b"hello"),
            other => panic!("unexpected result: {other:?}"),
        }
    }

    #[test]
    fn parses_chunked_request() {
        let raw = b"POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
        match try_parse_request(raw, 1024) {
            ParseResult::Complete { request, .. } => assert_eq!(request.body, b"Wikipedia"),
            other => panic!("unexpected result: {other:?}"),
        }
    }

    #[test]
    fn preserves_pipelined_request_after_empty_chunk_trailers() {
        let first = b"POST /upload HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n0\r\n\r\n";
        let second = b"GET /next HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        let mut raw = Vec::from(first.as_slice());
        raw.extend_from_slice(second);

        match try_parse_request(&raw, 4096) {
            ParseResult::Complete {
                request,
                consumed,
            } => {
                assert_eq!(request.body, b"Wiki");
                assert_eq!(consumed, first.len());
                assert_eq!(&raw[consumed..], second);
            }
            other => panic!("unexpected result: {other:?}"),
        }
    }

    #[test]
    fn rejects_conflicting_framing() {
        let raw = b"POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n";
        assert!(matches!(try_parse_request(raw, 1024), ParseResult::Error(_)));
    }
}
