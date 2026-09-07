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
        self.headers
            .get(&name.to_ascii_lowercase())
            .map(String::as_str)
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

#[derive(Debug, Clone)]
pub struct RequestHead {
    pub method: String,
    pub target: String,
    pub path: String,
    pub query: String,
    pub version: String,
    pub headers: HashMap<String, String>,
    pub body_start: usize,
}

impl RequestHead {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers
            .get(&name.to_ascii_lowercase())
            .map(String::as_str)
    }

    pub fn host(&self) -> &str {
        self.header("host").unwrap_or("")
    }

    pub fn content_length(&self) -> Option<usize> {
        self.header("content-length")
            .and_then(|value| value.parse::<usize>().ok())
    }

    pub fn is_chunked(&self) -> bool {
        self.header("transfer-encoding")
            .map(|encoding| {
                encoding
                    .split(',')
                    .map(str::trim)
                    .any(|token| token.eq_ignore_ascii_case("chunked"))
            })
            .unwrap_or(false)
    }

    pub fn into_request(self, body: Vec<u8>) -> Request {
        Request {
            method: self.method,
            target: self.target,
            path: self.path,
            query: self.query,
            version: self.version,
            headers: self.headers,
            body,
        }
    }
}

#[derive(Debug)]
pub enum HeadParseResult {
    NeedMore,
    Complete(RequestHead),
    Error(String),
    TooLarge(String),
}

#[derive(Debug)]
pub enum BodyParseResult {
    NeedMore,
    Complete { body: Vec<u8>, consumed: usize },
    Error(String),
    TooLarge(String),
}

#[derive(Debug)]
pub enum ParseResult {
    NeedMore,
    Complete { request: Request, consumed: usize },
    Error(String),
    TooLarge(String),
}

#[derive(Debug, Clone, Default)]
pub struct ChunkProgress {
    pos: usize,
    decoded_len: usize,
}

pub fn try_parse_request_head(buffer: &[u8], hard_limit: usize) -> HeadParseResult {
    let header_end = match find_bytes(buffer, b"\r\n\r\n") {
        Some(pos) => pos,
        None => {
            return if buffer.len() > hard_limit {
                HeadParseResult::TooLarge("request headers exceed hard server limit".into())
            } else {
                HeadParseResult::NeedMore
            }
        }
    };
    let body_start = header_end + 4;
    if body_start > hard_limit {
        return HeadParseResult::TooLarge("request headers exceed hard server limit".into());
    }

    let header_bytes = &buffer[..header_end];
    let header_text = match std::str::from_utf8(header_bytes) {
        Ok(value) => value,
        Err(_) => return HeadParseResult::Error("headers are not valid UTF-8".into()),
    };
    let mut lines = header_text.split("\r\n");
    let request_line = match lines.next() {
        Some(value) => value,
        None => return HeadParseResult::Error("missing request line".into()),
    };
    let request_parts: Vec<&str> = request_line.split_whitespace().collect();
    if request_parts.len() != 3 {
        return HeadParseResult::Error("invalid request line".into());
    }

    let method = request_parts[0].to_ascii_uppercase();
    if method.is_empty()
        || !method
            .bytes()
            .all(|byte| byte.is_ascii_uppercase() || byte == b'-')
    {
        return HeadParseResult::Error("invalid HTTP method".into());
    }
    if request_parts[2] != "HTTP/1.1" {
        return HeadParseResult::Error("only HTTP/1.1 is supported".into());
    }

    let target = request_parts[1].to_string();
    let (raw_path, query) = match target.split_once('?') {
        Some((path, query)) => (path.to_string(), query.to_string()),
        None => (target.clone(), String::new()),
    };
    if !raw_path.starts_with('/') {
        return HeadParseResult::Error("request target must use origin-form".into());
    }

    let mut headers = HashMap::new();
    for line in lines {
        if line.is_empty() {
            continue;
        }
        let (name, value) = match line.split_once(':') {
            Some(parts) => parts,
            None => return HeadParseResult::Error("malformed header line".into()),
        };
        let name = name.trim().to_ascii_lowercase();
        if name.is_empty()
            || !name
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'))
        {
            return HeadParseResult::Error("invalid header name".into());
        }
        let value = value.trim().to_string();
        if let Some(previous) = headers.get(&name) {
            if name == "content-length" && previous != &value {
                return HeadParseResult::Error("conflicting Content-Length headers".into());
            }
        }
        headers.insert(name, value);
    }

    if !headers.contains_key("host") {
        return HeadParseResult::Error("HTTP/1.1 Host header is required".into());
    }

    let transfer_encoding = headers
        .get("transfer-encoding")
        .map(|value| value.to_ascii_lowercase());
    let content_length = headers.get("content-length");
    if transfer_encoding.is_some() && content_length.is_some() {
        return HeadParseResult::Error(
            "Content-Length and Transfer-Encoding cannot be combined".into(),
        );
    }

    if let Some(encoding) = transfer_encoding {
        if !encoding
            .split(',')
            .map(str::trim)
            .any(|token| token.eq_ignore_ascii_case("chunked"))
        {
            return HeadParseResult::Error("unsupported Transfer-Encoding".into());
        }
    }

    if let Some(value) = content_length {
        let len = match value.parse::<usize>() {
            Ok(value) => value,
            Err(_) => return HeadParseResult::Error("invalid Content-Length".into()),
        };
        if len > hard_limit.saturating_sub(body_start) {
            return HeadParseResult::TooLarge("request body exceeds hard server limit".into());
        }
    }

    HeadParseResult::Complete(RequestHead {
        method,
        target,
        path: raw_path,
        query,
        version: "HTTP/1.1".into(),
        headers,
        body_start,
    })
}

pub fn try_parse_request_body(
    buffer: &[u8],
    head: &RequestHead,
    hard_limit: usize,
    body_limit: usize,
    chunk_progress: &mut ChunkProgress,
) -> BodyParseResult {
    if let Some(len) = head.content_length() {
        if len > body_limit {
            return BodyParseResult::TooLarge("request body exceeds configured limit".into());
        }
        let consumed = match head.body_start.checked_add(len) {
            Some(value) => value,
            None => return BodyParseResult::TooLarge("request body size overflow".into()),
        };
        if consumed > hard_limit {
            return BodyParseResult::TooLarge("request body exceeds hard server limit".into());
        }
        if buffer.len() < consumed {
            return BodyParseResult::NeedMore;
        }
        return BodyParseResult::Complete {
            body: buffer[head.body_start..consumed].to_vec(),
            consumed,
        };
    }

    if head.is_chunked() {
        if buffer.len() < head.body_start {
            return BodyParseResult::NeedMore;
        }
        let input = &buffer[head.body_start..];
        let wire_limit = hard_limit.saturating_sub(head.body_start);
        return match scan_chunked(input, chunk_progress, body_limit, wire_limit) {
            ChunkScanResult::NeedMore => BodyParseResult::NeedMore,
            ChunkScanResult::Complete { used } => {
                match decode_chunked_complete(input, used, chunk_progress.decoded_len) {
                    Ok(body) => BodyParseResult::Complete {
                        body,
                        consumed: head.body_start + used,
                    },
                    Err(error) => BodyParseResult::Error(error),
                }
            }
            ChunkScanResult::Error(error) => BodyParseResult::Error(error),
            ChunkScanResult::TooLarge(error) => BodyParseResult::TooLarge(error),
        };
    }

    BodyParseResult::Complete {
        body: Vec::new(),
        consumed: head.body_start,
    }
}

pub fn try_parse_request(buffer: &[u8], hard_limit: usize) -> ParseResult {
    let head = match try_parse_request_head(buffer, hard_limit) {
        HeadParseResult::NeedMore => return ParseResult::NeedMore,
        HeadParseResult::Error(error) => return ParseResult::Error(error),
        HeadParseResult::TooLarge(error) => return ParseResult::TooLarge(error),
        HeadParseResult::Complete(head) => head,
    };
    let mut progress = ChunkProgress::default();
    match try_parse_request_body(buffer, &head, hard_limit, hard_limit, &mut progress) {
        BodyParseResult::NeedMore => ParseResult::NeedMore,
        BodyParseResult::Error(error) => ParseResult::Error(error),
        BodyParseResult::TooLarge(error) => ParseResult::TooLarge(error),
        BodyParseResult::Complete { body, consumed } => ParseResult::Complete {
            request: head.into_request(body),
            consumed,
        },
    }
}

enum ChunkScanResult {
    NeedMore,
    Complete { used: usize },
    Error(String),
    TooLarge(String),
}

fn scan_chunked(
    input: &[u8],
    progress: &mut ChunkProgress,
    body_limit: usize,
    wire_limit: usize,
) -> ChunkScanResult {
    loop {
        if progress.pos > wire_limit {
            return ChunkScanResult::TooLarge("chunked request exceeds hard server limit".into());
        }
        let line_end = match find_bytes(&input[progress.pos..], b"\r\n") {
            Some(offset) => progress.pos + offset,
            None => return ChunkScanResult::NeedMore,
        };
        if line_end + 2 > wire_limit {
            return ChunkScanResult::TooLarge("chunked request exceeds hard server limit".into());
        }
        let line = match std::str::from_utf8(&input[progress.pos..line_end]) {
            Ok(value) => value,
            Err(_) => return ChunkScanResult::Error("invalid chunk size line".into()),
        };
        let size_text = line.split(';').next().unwrap_or("").trim();
        if size_text.is_empty() {
            return ChunkScanResult::Error("empty chunk size".into());
        }
        let size = match usize::from_str_radix(size_text, 16) {
            Ok(value) => value,
            Err(_) => return ChunkScanResult::Error("invalid hexadecimal chunk size".into()),
        };
        let data_start = line_end + 2;

        if size == 0 {
            if data_start + 2 > wire_limit {
                return ChunkScanResult::TooLarge(
                    "chunked request exceeds hard server limit".into(),
                );
            }
            if input.get(data_start..data_start + 2) == Some(b"\r\n") {
                return ChunkScanResult::Complete {
                    used: data_start + 2,
                };
            }
            let trailer_end = match find_bytes(&input[data_start..], b"\r\n\r\n") {
                Some(offset) => data_start + offset + 4,
                None => return ChunkScanResult::NeedMore,
            };
            if trailer_end > wire_limit {
                return ChunkScanResult::TooLarge(
                    "chunked request exceeds hard server limit".into(),
                );
            }
            return ChunkScanResult::Complete { used: trailer_end };
        }

        if progress.decoded_len.saturating_add(size) > body_limit {
            return ChunkScanResult::TooLarge("decoded chunked body exceeds configured limit".into());
        }
        let chunk_end = match data_start.checked_add(size) {
            Some(value) => value,
            None => return ChunkScanResult::TooLarge("chunk size overflow".into()),
        };
        let framed_end = match chunk_end.checked_add(2) {
            Some(value) => value,
            None => return ChunkScanResult::TooLarge("chunk size overflow".into()),
        };
        if framed_end > wire_limit {
            return ChunkScanResult::TooLarge("chunked request exceeds hard server limit".into());
        }
        if input.len() < framed_end {
            return ChunkScanResult::NeedMore;
        }
        if input.get(chunk_end..framed_end) != Some(b"\r\n") {
            return ChunkScanResult::Error("chunk data is not followed by CRLF".into());
        }
        progress.decoded_len += size;
        progress.pos = framed_end;
    }
}

fn decode_chunked_complete(
    input: &[u8],
    used: usize,
    decoded_len: usize,
) -> Result<Vec<u8>, String> {
    let mut out = Vec::with_capacity(decoded_len);
    let mut pos = 0usize;
    while pos < used {
        let line_end = find_bytes(&input[pos..used], b"\r\n")
            .map(|offset| pos + offset)
            .ok_or_else(|| "invalid chunk framing".to_string())?;
        let line = std::str::from_utf8(&input[pos..line_end])
            .map_err(|_| "invalid chunk size line".to_string())?;
        let size_text = line.split(';').next().unwrap_or("").trim();
        let size = usize::from_str_radix(size_text, 16)
            .map_err(|_| "invalid hexadecimal chunk size".to_string())?;
        pos = line_end + 2;
        if size == 0 {
            break;
        }
        let end = pos
            .checked_add(size)
            .ok_or_else(|| "chunk size overflow".to_string())?;
        if end + 2 > used || input.get(end..end + 2) != Some(b"\r\n") {
            return Err("invalid chunk framing".into());
        }
        out.extend_from_slice(&input[pos..end]);
        pos = end + 2;
    }
    Ok(out)
}

pub fn find_bytes(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() {
        return Some(0);
    }
    haystack
        .windows(needle.len())
        .position(|window| window == needle)
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

        let no_entity = (100..200).contains(&self.status) || matches!(self.status, 204 | 304);
        let mut has_length = false;
        let mut has_type = false;
        let mut has_connection = false;
        for (name, value) in &self.headers {
            if no_entity
                && (name.eq_ignore_ascii_case("content-length")
                    || name.eq_ignore_ascii_case("content-type"))
            {
                continue;
            }
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
        if !no_entity && !has_type {
            out.extend_from_slice(b"Content-Type: text/plain; charset=utf-8\r\n");
        }
        if !no_entity && !has_length {
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
        if !no_entity {
            out.extend_from_slice(&self.body);
        }
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
        .and_then(|value| value.to_str())
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
        assert!(matches!(
            try_parse_request(raw, 1024),
            ParseResult::Error(_)
        ));
    }

    #[test]
    fn chunked_limit_is_enforced_incrementally() {
        let raw = b"POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n10\r\n";
        let head = match try_parse_request_head(raw, 4096) {
            HeadParseResult::Complete(head) => head,
            other => panic!("unexpected head result: {other:?}"),
        };
        let mut progress = ChunkProgress::default();
        assert!(matches!(
            try_parse_request_body(raw, &head, 4096, 8, &mut progress),
            BodyParseResult::TooLarge(_)
        ));
    }

    #[test]
    fn serializes_204_without_entity_headers() {
        let bytes = Response::new(204, b"ignored".to_vec()).to_bytes(false);
        let text = String::from_utf8(bytes).unwrap();
        assert!(!text.to_ascii_lowercase().contains("content-length:"));
        assert!(!text.to_ascii_lowercase().contains("content-type:"));
        assert!(text.ends_with("\r\n\r\n"));
    }
}
