// src/core/mcp_launch.cpp — see mcp_launch.h
#include "mcp_launch.h"
#include "cgroup.h"
#include "sandbox.h"
#include "tools/process_runner.h"
#include <iostream>

namespace funes {

std::unique_ptr<mcp::stdio_client> make_stdio_client(const McpServerConfig& srv,
                                                     const std::string& agent_name) {
    auto client = std::make_unique<mcp::stdio_client>(srv.command, srv.env);

    sandbox::Policy policy = sandbox::base();
    policy.read.insert(policy.read.end(), srv.access.read.begin(), srv.access.read.end());
    policy.exec.insert(policy.exec.end(), srv.access.exec.begin(), srv.access.exec.end());
    policy.write.insert(policy.write.end(), srv.access.write.begin(), srv.access.write.end());
    policy.connect = srv.access.connect;

    const std::string label = srv.name.empty() ? srv.command : srv.name;
    client->set_launch_wrapper([policy, agent_name, label](const std::vector<std::string>& argv) {
        // Shared: the plan's on_exit owns it until the server is reaped.
        std::shared_ptr<proc::Wrapped> w = proc::wrap(argv, std::nullopt, policy);
        mcp::stdio_client::launch_plan plan;
        plan.argv = w->argv;
        if (!w->tmpdir.empty()) plan.env.emplace_back("TMPDIR", w->tmpdir);
        plan.on_exit = [w, agent_name, label] {
            if (auto u = w->finish())
                std::cerr << "[agent:" << agent_name << "] MCP server '" << label
                          << "' stopped" << cgroup::format_usage(*u) << "\n";
        };
        return plan;
    });
    return client;
}

} // namespace funes
