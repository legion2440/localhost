use std::path::{Component, Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

pub fn percent_decode(input: &str) -> Result<String, String> {
    let bytes = input.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' {
            if i + 2 >= bytes.len() {
                return Err("truncated percent escape".into());
            }
            let hi = hex(bytes[i + 1]).ok_or_else(|| "invalid percent escape".to_string())?;
            let lo = hex(bytes[i + 2]).ok_or_else(|| "invalid percent escape".to_string())?;
            out.push((hi << 4) | lo);
            i += 3;
        } else {
            out.push(bytes[i]);
            i += 1;
        }
    }
    String::from_utf8(out).map_err(|_| "URL path is not valid UTF-8".into())
}

fn hex(b: u8) -> Option<u8> {
    match b {
        b'0'..=b'9' => Some(b - b'0'),
        b'a'..=b'f' => Some(10 + b - b'a'),
        b'A'..=b'F' => Some(10 + b - b'A'),
        _ => None,
    }
}

pub fn normalize_url_path(path: &str) -> Result<String, String> {
    let decoded = percent_decode(path)?;
    if !decoded.starts_with('/') || decoded.contains('\0') {
        return Err("invalid request path".into());
    }
    let mut parts = Vec::new();
    for part in decoded.split('/') {
        match part {
            "" | "." => {}
            ".." => return Err("parent path traversal is forbidden".into()),
            other => parts.push(other),
        }
    }
    Ok(format!("/{}", parts.join("/")))
}

pub fn safe_join(root: &Path, relative: &str) -> Result<PathBuf, String> {
    let mut path = root.to_path_buf();
    for component in Path::new(relative).components() {
        match component {
            Component::Normal(part) => path.push(part),
            Component::CurDir => {}
            _ => return Err("unsafe filesystem path".into()),
        }
    }
    Ok(path)
}

pub fn sanitize_filename(name: &str) -> Option<String> {
    let trimmed = name.trim().replace('\\', "/");
    let base = trimmed.rsplit('/').next()?.trim();
    if base.is_empty() || base == "." || base == ".." || base.contains('\0') {
        return None;
    }
    let safe: String = base
        .chars()
        .map(|ch| {
            if ch.is_ascii_alphanumeric() || matches!(ch, '.' | '-' | '_') {
                ch
            } else {
                '_'
            }
        })
        .collect();
    if safe.is_empty() { None } else { Some(safe) }
}

pub fn html_escape(input: &str) -> String {
    input
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&#39;")
}

pub fn now_millis() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_encoded_parent_traversal() {
        assert!(normalize_url_path("/%2e%2e/etc/passwd").is_err());
    }

    #[test]
    fn sanitizes_upload_name() {
        assert_eq!(sanitize_filename("../../hello world.txt").unwrap(), "hello_world.txt");
    }
}
