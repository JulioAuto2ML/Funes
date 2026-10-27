# third-party/

The MCP servers the WhatsApp and Gmail agents spawn. They were vendored in the
Funes tree until 5.1 and are moved here with a plain `git mv` from the Funes
checkout (they are large and had no code changes in the split):

```bash
cd ../Funes
git mv third-party/whatsapp-mcp            ../funes-julio/third-party/
git mv third-party/imap-email-mcp-patched  ../funes-julio/third-party/
```

| Directory | What |
|---|---|
| `imap-email-mcp-patched/` | Node.js IMAP/SMTP MCP server, patched: the published npm package never sets the TLS SNI `servername`, so Gmail's IMAP frontend returns a self-signed fallback cert. The patch is in `patches/`. |
| `whatsapp-mcp/` | Go + Python WhatsApp Web bridge + MCP tools, a patched copy of [lharries/whatsapp-mcp](https://github.com/lharries/whatsapp-mcp). REST API port moved from 8080 to 8090 to avoid llama-server. |

Both run as child processes of the agents' `mcp_servers:` entries; nothing here
is compiled into the binary. Since Funes 5.0.1 a spawned server gets an
allowlisted environment, so the credentials they need go in the agent YAML's
`env:` block (see `config/funes.conf.example`), not in the server's environment.
