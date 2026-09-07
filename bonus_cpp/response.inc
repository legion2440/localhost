static Response text_response(
    int status,
    const std::string& text,
    const std::string& type = "text/plain; charset=utf-8"
) {
    Response response;
    response.status = status;
    response.body.assign(text.begin(), text.end());
    response.headers.push_back({"Content-Type", type});
    return response;
}

static std::vector<char> serialize(const Response& response, bool keep_alive) {
    std::ostringstream head;
    head << "HTTP/1.1 " << response.status << ' ' << reason(response.status) << "\r\n";

    const bool no_entity = (response.status >= 100 && response.status < 200)
        || response.status == 204
        || response.status == 304;
    bool has_length = false;
    bool has_type = false;
    bool has_connection = false;
    for (const auto& header : response.headers) {
        const std::string key = lower(header.first);
        if (no_entity && (key == "content-length" || key == "content-type")) {
            continue;
        }
        has_length = has_length || key == "content-length";
        has_type = has_type || key == "content-type";
        has_connection = has_connection || key == "connection";
        head << header.first << ": " << header.second << "\r\n";
    }
    if (!no_entity && !has_type) {
        head << "Content-Type: text/plain; charset=utf-8\r\n";
    }
    if (!no_entity && !has_length) {
        head << "Content-Length: " << response.body.size() << "\r\n";
    }
    if (!has_connection) {
        head << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";
    }
    head << "Server: localhost-cpp/0.1\r\n\r\n";

    const std::string head_text = head.str();
    std::vector<char> output(head_text.begin(), head_text.end());
    if (!no_entity) {
        output.insert(output.end(), response.body.begin(), response.body.end());
    }
    return output;
}
