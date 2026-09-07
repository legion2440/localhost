enum class ParseKind {
    NeedMore,
    Complete,
    Error,
    TooLarge,
};

struct HeadParseOut {
    ParseKind kind = ParseKind::NeedMore;
    RequestHead head;
    std::string error;
};

struct BodyParseOut {
    ParseKind kind = ParseKind::NeedMore;
    std::vector<char> body;
    size_t consumed = 0;
    std::string error;
};

static HeadParseOut parse_request_head(const std::vector<char>& bytes) {
    HeadParseOut output;
    const auto header_end = find_bytes(bytes, 0, "\r\n\r\n");
    if (!header_end) {
        if (bytes.size() > HARD_LIMIT) {
            output.kind = ParseKind::TooLarge;
            output.error = "request headers exceed hard server limit";
        }
        return output;
    }

    output.head.body_start = *header_end + 4;
    if (output.head.body_start > HARD_LIMIT) {
        output.kind = ParseKind::TooLarge;
        output.error = "request headers exceed hard server limit";
        return output;
    }

    const std::string headers_text(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(*header_end));
    std::istringstream input(headers_text);
    std::string line;
    if (!std::getline(input, line)) {
        output.kind = ParseKind::Error;
        output.error = "missing request line";
        return output;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    const auto parts = split_ws(line);
    if (parts.size() != 3 || parts[2] != "HTTP/1.1") {
        output.kind = ParseKind::Error;
        output.error = "bad request line";
        return output;
    }

    output.head.method = parts[0];
    std::transform(
        output.head.method.begin(),
        output.head.method.end(),
        output.head.method.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); }
    );
    if (output.head.method.empty()
        || !std::all_of(output.head.method.begin(), output.head.method.end(), [](unsigned char c) {
            return std::isupper(c) || c == '-';
        })) {
        output.kind = ParseKind::Error;
        output.error = "invalid HTTP method";
        return output;
    }

    output.head.target = parts[1];
    output.head.version = parts[2];
    const auto query = parts[1].find('?');
    output.head.path = parts[1].substr(0, query);
    output.head.query = query == std::string::npos ? "" : parts[1].substr(query + 1);
    if (output.head.path.empty() || output.head.path.front() != '/') {
        output.kind = ParseKind::Error;
        output.error = "request target must use origin-form";
        return output;
    }

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            output.kind = ParseKind::Error;
            output.error = "bad header";
            return output;
        }
        const std::string key = lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (key.empty() || !std::all_of(key.begin(), key.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_';
        })) {
            output.kind = ParseKind::Error;
            output.error = "invalid header name";
            return output;
        }
        const auto existing = output.head.headers.find(key);
        if (key == "content-length"
            && existing != output.head.headers.end()
            && existing->second != value) {
            output.kind = ParseKind::Error;
            output.error = "conflicting content-length";
            return output;
        }
        output.head.headers[key] = value;
    }

    if (output.head.headers.find("host") == output.head.headers.end()) {
        output.kind = ParseKind::Error;
        output.error = "Host required";
        return output;
    }

    const std::string transfer_encoding = lower(output.head.header("transfer-encoding"));
    const std::string content_length = output.head.header("content-length");
    if (!transfer_encoding.empty() && !content_length.empty()) {
        output.kind = ParseKind::Error;
        output.error = "conflicting framing";
        return output;
    }
    if (!transfer_encoding.empty() && !output.head.is_chunked()) {
        output.kind = ParseKind::Error;
        output.error = "unsupported transfer-encoding";
        return output;
    }
    if (!content_length.empty()) {
        try {
            const size_t length = std::stoull(content_length);
            if (length > HARD_LIMIT - output.head.body_start) {
                output.kind = ParseKind::TooLarge;
                output.error = "request body exceeds hard server limit";
                return output;
            }
        } catch (...) {
            output.kind = ParseKind::Error;
            output.error = "bad content-length";
            return output;
        }
    }

    output.kind = ParseKind::Complete;
    return output;
}

enum class ChunkScanKind {
    NeedMore,
    Complete,
    Error,
    TooLarge,
};

struct ChunkScanOut {
    ChunkScanKind kind = ChunkScanKind::NeedMore;
    size_t used = 0;
    std::string error;
};

static ChunkScanOut scan_chunked(
    const std::vector<char>& bytes,
    size_t body_start,
    ChunkProgress& progress,
    size_t body_limit
) {
    ChunkScanOut output;
    const size_t wire_limit = HARD_LIMIT - body_start;

    for (;;) {
        if (progress.pos > wire_limit) {
            output.kind = ChunkScanKind::TooLarge;
            output.error = "chunked request exceeds hard server limit";
            return output;
        }

        const auto line_end_abs = find_bytes(bytes, body_start + progress.pos, "\r\n");
        if (!line_end_abs) {
            return output;
        }
        if (*line_end_abs < body_start) {
            output.kind = ChunkScanKind::Error;
            output.error = "invalid chunk framing";
            return output;
        }
        const size_t line_end = *line_end_abs - body_start;
        if (line_end + 2 > wire_limit) {
            output.kind = ChunkScanKind::TooLarge;
            output.error = "chunked request exceeds hard server limit";
            return output;
        }

        std::string size_text(
            bytes.begin() + static_cast<std::ptrdiff_t>(body_start + progress.pos),
            bytes.begin() + static_cast<std::ptrdiff_t>(*line_end_abs)
        );
        const auto semicolon = size_text.find(';');
        if (semicolon != std::string::npos) {
            size_text = size_text.substr(0, semicolon);
        }
        size_text = trim(size_text);
        if (size_text.empty()) {
            output.kind = ChunkScanKind::Error;
            output.error = "empty chunk size";
            return output;
        }

        size_t size = 0;
        try {
            size = std::stoull(size_text, nullptr, 16);
        } catch (...) {
            output.kind = ChunkScanKind::Error;
            output.error = "bad chunk size";
            return output;
        }
        const size_t data_start = line_end + 2;

        if (size == 0) {
            if (data_start + 2 > wire_limit) {
                output.kind = ChunkScanKind::TooLarge;
                output.error = "chunked request exceeds hard server limit";
                return output;
            }
            const size_t absolute_data_start = body_start + data_start;
            if (bytes.size() >= absolute_data_start + 2
                && std::string(
                    bytes.begin() + static_cast<std::ptrdiff_t>(absolute_data_start),
                    bytes.begin() + static_cast<std::ptrdiff_t>(absolute_data_start + 2)
                ) == "\r\n") {
                output.kind = ChunkScanKind::Complete;
                output.used = data_start + 2;
                return output;
            }

            const auto trailer_end = find_bytes(bytes, absolute_data_start, "\r\n\r\n");
            if (!trailer_end) {
                return output;
            }
            output.used = *trailer_end - body_start + 4;
            if (output.used > wire_limit) {
                output.kind = ChunkScanKind::TooLarge;
                output.error = "chunked request exceeds hard server limit";
                return output;
            }
            output.kind = ChunkScanKind::Complete;
            return output;
        }

        if (size > body_limit - std::min(progress.decoded_len, body_limit)) {
            output.kind = ChunkScanKind::TooLarge;
            output.error = "decoded chunked body exceeds configured limit";
            return output;
        }

        if (size > wire_limit - std::min(data_start, wire_limit)) {
            output.kind = ChunkScanKind::TooLarge;
            output.error = "chunked request exceeds hard server limit";
            return output;
        }
        const size_t chunk_end = data_start + size;
        if (chunk_end > wire_limit - 2) {
            output.kind = ChunkScanKind::TooLarge;
            output.error = "chunked request exceeds hard server limit";
            return output;
        }
        const size_t framed_end = chunk_end + 2;
        const size_t absolute_framed_end = body_start + framed_end;
        if (bytes.size() < absolute_framed_end) {
            return output;
        }
        if (std::string(
                bytes.begin() + static_cast<std::ptrdiff_t>(body_start + chunk_end),
                bytes.begin() + static_cast<std::ptrdiff_t>(absolute_framed_end)
            ) != "\r\n") {
            output.kind = ChunkScanKind::Error;
            output.error = "bad chunk terminator";
            return output;
        }
        progress.decoded_len += size;
        progress.pos = framed_end;
    }
}

static std::optional<std::vector<char>> decode_chunked_complete(
    const std::vector<char>& bytes,
    size_t body_start,
    size_t used,
    size_t decoded_len
) {
    std::vector<char> body;
    body.reserve(decoded_len);
    size_t pos = 0;
    while (pos < used) {
        const auto line_end_abs = find_bytes(bytes, body_start + pos, "\r\n");
        if (!line_end_abs || *line_end_abs < body_start) {
            return std::nullopt;
        }
        const size_t line_end = *line_end_abs - body_start;
        std::string size_text(
            bytes.begin() + static_cast<std::ptrdiff_t>(body_start + pos),
            bytes.begin() + static_cast<std::ptrdiff_t>(*line_end_abs)
        );
        const auto semicolon = size_text.find(';');
        if (semicolon != std::string::npos) {
            size_text = size_text.substr(0, semicolon);
        }
        size_t size = 0;
        try {
            size = std::stoull(trim(size_text), nullptr, 16);
        } catch (...) {
            return std::nullopt;
        }
        pos = line_end + 2;
        if (size == 0) {
            break;
        }
        if (pos + size + 2 > used) {
            return std::nullopt;
        }
        body.insert(
            body.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(body_start + pos),
            bytes.begin() + static_cast<std::ptrdiff_t>(body_start + pos + size)
        );
        pos += size;
        if (std::string(
                bytes.begin() + static_cast<std::ptrdiff_t>(body_start + pos),
                bytes.begin() + static_cast<std::ptrdiff_t>(body_start + pos + 2)
            ) != "\r\n") {
            return std::nullopt;
        }
        pos += 2;
    }
    return body;
}

static BodyParseOut parse_request_body(
    const std::vector<char>& bytes,
    const RequestHead& head,
    ChunkProgress& chunk,
    size_t body_limit
) {
    BodyParseOut output;

    if (const auto content_length = head.content_length()) {
        if (*content_length > body_limit) {
            output.kind = ParseKind::TooLarge;
            output.error = "request body exceeds configured limit";
            return output;
        }
        const size_t consumed = head.body_start + *content_length;
        if (consumed > HARD_LIMIT) {
            output.kind = ParseKind::TooLarge;
            output.error = "request body exceeds hard server limit";
            return output;
        }
        if (bytes.size() < consumed) {
            return output;
        }
        output.body.assign(
            bytes.begin() + static_cast<std::ptrdiff_t>(head.body_start),
            bytes.begin() + static_cast<std::ptrdiff_t>(consumed)
        );
        output.consumed = consumed;
        output.kind = ParseKind::Complete;
        return output;
    }

    if (head.is_chunked()) {
        const ChunkScanOut scan = scan_chunked(bytes, head.body_start, chunk, body_limit);
        if (scan.kind == ChunkScanKind::NeedMore) {
            return output;
        }
        if (scan.kind == ChunkScanKind::Error) {
            output.kind = ParseKind::Error;
            output.error = scan.error;
            return output;
        }
        if (scan.kind == ChunkScanKind::TooLarge) {
            output.kind = ParseKind::TooLarge;
            output.error = scan.error;
            return output;
        }
        const auto body = decode_chunked_complete(bytes, head.body_start, scan.used, chunk.decoded_len);
        if (!body) {
            output.kind = ParseKind::Error;
            output.error = "invalid chunk framing";
            return output;
        }
        output.body = *body;
        output.consumed = head.body_start + scan.used;
        output.kind = ParseKind::Complete;
        return output;
    }

    output.consumed = head.body_start;
    output.kind = ParseKind::Complete;
    return output;
}
