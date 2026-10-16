// =============================================================================
// tests/test_mcp_http_client.cpp — streamable-HTTP MCP client against a local mock
// =============================================================================

#include "mcp_http_client.h"
#include <httplib.h>
#include <iostream>
#include <thread>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

using mcp::json;

int main() {
    // URL parsing: path and query survive, defaults by scheme, junk refused.
    auto u = funes::parse_mcp_url("https://monitorargy.com/mcp");
    CHECK(u.scheme == "https" && u.host == "monitorargy.com" && u.port == 443 && u.path == "/mcp");
    u = funes::parse_mcp_url("http://localhost:9000");
    CHECK(u.port == 9000 && u.path == "/");
    u = funes::parse_mcp_url("http://h/mcp?k=v");
    CHECK(u.path == "/mcp?k=v");
    bool threw = false;
    try { funes::parse_mcp_url("ftp://x/mcp"); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);

    // SSE body: picks the event carrying our id, skips notifications.
    const std::string sse =
        "event: message\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}\n\n"
        "event: message\r\ndata: {\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"ok\":true}}\r\n\r\n";
    CHECK(funes::find_sse_response(sse, 7)["result"]["ok"] == true);
    CHECK(funes::find_sse_response(sse, 8).is_null());

    // Mock server: session id issued on initialize and required afterwards;
    // tools/list answers as JSON, tools/call answers as an SSE stream.
    httplib::Server svr;
    int notifications = 0;
    bool session_ok = true;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        json m = json::parse(req.body);
        const std::string method = m.value("method", "");
        if (method != "initialize" && req.get_header_value("Mcp-Session-Id") != "sess-1")
            session_ok = false;
        if (!m.contains("id")) { ++notifications; res.status = 202; return; }
        if (method == "initialize") {
            res.set_header("Mcp-Session-Id", "sess-1");
            res.set_content(json{{"jsonrpc","2.0"},{"id",m["id"]},{"result",
                {{"capabilities",{{"tools",json::object()}}}}}}.dump(), "application/json");
        } else if (method == "tools/list") {
            res.set_content(json{{"jsonrpc","2.0"},{"id",m["id"]},{"result",{{"tools",
                json::array({{{"name","coyuntura"},{"description","d"},
                              {"inputSchema",{{"type","object"}}}}})}}}}.dump(), "application/json");
        } else if (method == "tools/call") {
            json r = {{"jsonrpc","2.0"},{"id",m["id"]},{"result",
                {{"content", json::array({{{"type","text"},{"text","hola"}}})}}}};
            res.set_content("event: message\ndata: " + r.dump() + "\n\n", "text/event-stream");
        } else {
            res.set_content(json{{"jsonrpc","2.0"},{"id",m["id"]},{"error",
                {{"code",-32601},{"message","nope"}}}}.dump(), "application/json");
        }
    });
    const int port = svr.bind_to_any_port("127.0.0.1");
    std::thread t([&] { svr.listen_after_bind(); });
    svr.wait_until_ready();

    funes::HttpMcpClient c("http://127.0.0.1:" + std::to_string(port) + "/mcp", 5);
    CHECK(c.initialize("test", "1"));
    CHECK(c.is_running());
    CHECK(notifications == 1);
    auto tools = c.get_tools();
    CHECK(tools.size() == 1 && tools[0].name == "coyuntura");
    json out = c.call_tool("coyuntura", json::object());
    CHECK(out["content"][0]["text"] == "hola");
    CHECK(session_ok);

    threw = false;
    try { c.send_request("bogus"); } catch (const std::exception& e) {
        threw = std::string(e.what()).find("nope") != std::string::npos;
    }
    CHECK(threw);

    svr.stop();
    t.join();

    // Unreachable server: initialize reports false rather than throwing.
    funes::HttpMcpClient dead("http://127.0.0.1:" + std::to_string(port) + "/mcp", 2);
    CHECK(!dead.initialize("test", "1"));

    std::cout << "All tests passed.\n";
    return 0;
}
