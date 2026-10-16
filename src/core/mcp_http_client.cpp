#include "mcp_http_client.h"

#include <httplib.h>

#include <regex>
#include <sstream>
#include <stdexcept>

namespace funes {

using mcp::json;

ParsedUrl parse_mcp_url(const std::string& url) {
    static const std::regex re(R"(^(https?)://([^/:?#]+)(?::(\d+))?(/[^?#]*)?(\?[^#]*)?$)");
    std::smatch m;
    if (!std::regex_match(url, m, re))
        throw std::runtime_error("Invalid MCP server URL: " + url);
    ParsedUrl u;
    u.scheme = m[1].str();
    u.host   = m[2].str();
    u.port   = m[3].matched ? std::stoi(m[3].str()) : (u.scheme == "https" ? 443 : 80);
    u.path   = (m[4].matched ? m[4].str() : "/") + (m[5].matched ? m[5].str() : "");
    return u;
}

json find_sse_response(const std::string& body, const json& id) {
    std::istringstream in(body);
    std::string line, data;
    auto flush = [&]() -> json {
        if (data.empty()) return nullptr;
        json msg = json::parse(data, nullptr, /*allow_exceptions=*/false);
        data.clear();
        if (msg.is_object() && msg.contains("id") && msg["id"] == id
            && (msg.contains("result") || msg.contains("error")))
            return msg;
        return nullptr;
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {                       // blank line ends one event
            json hit = flush();
            if (!hit.is_null()) return hit;
        } else if (line.rfind("data:", 0) == 0) {
            std::string v = line.substr(5);
            if (!v.empty() && v.front() == ' ') v.erase(0, 1);
            if (!data.empty()) data += '\n';
            data += v;
        }
    }
    return flush();
}

HttpMcpClient::HttpMcpClient(const std::string& url, int timeout_seconds)
    : url_(parse_mcp_url(url)), timeout_(timeout_seconds) {}

std::string HttpMcpClient::post(const json& message, std::string& content_type) {
    httplib::Client cli(url_.scheme + "://" + url_.host + ":" + std::to_string(url_.port));
    cli.set_connection_timeout(10);
    cli.set_read_timeout(timeout_);
    cli.set_write_timeout(timeout_);
    // Redirects stay off: they would skip any host check the caller made on the URL.
    cli.set_follow_location(false);

    httplib::Headers headers = {
        {"Accept", "application/json, text/event-stream"},
        {"MCP-Protocol-Version", "2025-03-26"},
    };
    if (!session_id_.empty()) headers.emplace("Mcp-Session-Id", session_id_);

    auto res = cli.Post(url_.path, headers, message.dump(), "application/json");
    if (!res)
        throw mcp::mcp_exception(mcp::error_code::internal_error,
            "MCP HTTP request failed: " + httplib::to_string(res.error()));
    if (res->status == 404 && !session_id_.empty()) {
        session_id_.clear();
        initialized_ = false;
        throw mcp::mcp_exception(mcp::error_code::internal_error, "MCP session expired");
    }
    if (res->status < 200 || res->status >= 300)
        throw mcp::mcp_exception(mcp::error_code::internal_error,
            "MCP HTTP status " + std::to_string(res->status));

    if (res->has_header("Mcp-Session-Id")) session_id_ = res->get_header_value("Mcp-Session-Id");
    content_type = res->get_header_value("Content-Type");
    return res->body;
}

mcp::response HttpMcpClient::send_request(const std::string& method, const json& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int id = next_id_++;
    json msg = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.is_null()) msg["params"] = params;

    std::string content_type;
    const std::string body = post(msg, content_type);

    json reply;
    if (content_type.find("text/event-stream") != std::string::npos)
        reply = find_sse_response(body, id);
    else
        reply = json::parse(body, nullptr, /*allow_exceptions=*/false);

    if (!reply.is_object())
        throw mcp::mcp_exception(mcp::error_code::parse_error,
            "MCP server sent no usable reply to '" + method + "'");
    if (reply.contains("error") && !reply["error"].is_null()) {
        const json& e = reply["error"];
        throw mcp::mcp_exception(mcp::error_code::internal_error,
            e.value("message", std::string("MCP error")));
    }
    mcp::response r;
    r.id     = reply.value("id", json(id));
    r.result = reply.contains("result") ? reply["result"] : json::object();
    return r;
}

void HttpMcpClient::send_notification(const std::string& method, const json& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    json msg = {{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.is_null() && !params.empty()) msg["params"] = params;
    std::string content_type;
    post(msg, content_type);   // 202 Accepted, no body
}

bool HttpMcpClient::initialize(const std::string& client_name, const std::string& client_version) {
    try {
        mcp::response r = send_request("initialize", {
            {"protocolVersion", "2025-03-26"},
            {"capabilities", capabilities_},
            {"clientInfo", {{"name", client_name}, {"version", client_version}}},
        });
        server_capabilities_ = r.result.value("capabilities", json::object());
        send_notification("notifications/initialized");
        std::lock_guard<std::mutex> lock(mutex_);
        initialized_ = true;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool HttpMcpClient::ping() {
    try { send_request("ping"); return true; } catch (const std::exception&) { return false; }
}

json HttpMcpClient::call_tool(const std::string& tool_name, const json& arguments) {
    return send_request("tools/call", {{"name", tool_name}, {"arguments", arguments}}).result;
}

std::vector<mcp::tool> HttpMcpClient::get_tools() {
    std::vector<mcp::tool> tools;
    const json result = send_request("tools/list").result;
    if (!result.contains("tools") || !result["tools"].is_array()) return tools;
    for (const auto& tj : result["tools"]) {
        if (!tj.is_object() || !tj.contains("name") || !tj["name"].is_string()) continue;
        mcp::tool t;
        t.name        = tj["name"].get<std::string>();
        t.description = tj.value("description", std::string());
        if (tj.contains("inputSchema")) t.parameters_schema = tj["inputSchema"];
        tools.push_back(std::move(t));
    }
    return tools;
}

json HttpMcpClient::list_resources(const std::string& cursor) {
    json p = json::object();
    if (!cursor.empty()) p["cursor"] = cursor;
    return send_request("resources/list", p).result;
}
json HttpMcpClient::read_resource(const std::string& uri) {
    return send_request("resources/read", {{"uri", uri}}).result;
}
json HttpMcpClient::subscribe_to_resource(const std::string& uri) {
    return send_request("resources/subscribe", {{"uri", uri}}).result;
}
json HttpMcpClient::list_resource_templates() {
    return send_request("resources/templates/list").result;
}

} // namespace funes
