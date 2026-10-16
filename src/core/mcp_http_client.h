// =============================================================================
// mcp_http_client.h — MCP "streamable HTTP" transport (spec 2025-03-26)
//
// The vendored cpp-mcp only speaks the older two-endpoint SSE transport
// (GET /sse, then POST to a session URL). Current hosted MCP servers expose one
// URL that takes a JSON-RPC POST and answers with either application/json or a
// short text/event-stream. This is the client for that: no background thread,
// one request in flight per call, Mcp-Session-Id carried if the server issues one.
// =============================================================================
#pragma once

#include "mcp_client.h"

#include <mutex>
#include <string>

namespace funes {

struct ParsedUrl {
    std::string scheme;   // "http" | "https"
    std::string host;
    int         port = 0;
    std::string path;     // always starts with '/'
};

// Throws std::runtime_error on anything that is not http(s)://host[:port][/path].
ParsedUrl parse_mcp_url(const std::string& url);

// The JSON-RPC message with the given id from a text/event-stream body, or a
// null json if none carries it.
mcp::json find_sse_response(const std::string& body, const mcp::json& id);

class HttpMcpClient : public mcp::client {
public:
    explicit HttpMcpClient(const std::string& url, int timeout_seconds = 60);

    bool initialize(const std::string& client_name, const std::string& client_version) override;
    bool ping() override;
    void set_capabilities(const mcp::json& capabilities) override { capabilities_ = capabilities; }
    mcp::response send_request(const std::string& method, const mcp::json& params = mcp::json::object()) override;
    void send_notification(const std::string& method, const mcp::json& params = mcp::json::object()) override;
    mcp::json get_server_capabilities() override { return server_capabilities_; }
    mcp::json call_tool(const std::string& tool_name, const mcp::json& arguments = mcp::json::object()) override;
    std::vector<mcp::tool> get_tools() override;
    mcp::json get_capabilities() override { return capabilities_; }
    mcp::json list_resources(const std::string& cursor = "") override;
    mcp::json read_resource(const std::string& resource_uri) override;
    mcp::json subscribe_to_resource(const std::string& resource_uri) override;
    mcp::json list_resource_templates() override;
    bool is_running() const override { return initialized_; }

private:
    // POSTs one message; returns the reply body ("" for a 202) and its content type.
    std::string post(const mcp::json& message, std::string& content_type);

    ParsedUrl   url_;
    int         timeout_;
    std::mutex  mutex_;          // one request at a time; also guards session_id_
    std::string session_id_;
    bool        initialized_ = false;
    int         next_id_ = 1;
    mcp::json   capabilities_ = mcp::json::object();
    mcp::json   server_capabilities_ = mcp::json::object();
};

} // namespace funes
