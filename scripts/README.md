# scripts/

Operational scripts and systemd service files for running Funes and its
companion services.

## Scripts

| Script | Purpose |
|---|---|
| `funesctl.sh` | Thin wrapper around `systemctl --user start/stop/restart/status funes.service`. |
| `whatsapp_autoresponder.py` | Polls the WhatsApp bridge for incoming messages, asks the `whatsapp-autoresponder` agent for a reply, sends it back. |
| `whatsapp_whitelist.py` | CLI tool for managing the autoresponder contact whitelist (`list/add/remove`). |

## WhatsApp autoresponder

A standalone Python service (stdlib only, no external dependencies) that runs
as a systemd user service. The safety model is that the LLM never chooses a
recipient:

1. **Polls** the WhatsApp bridge's SQLite database for new messages from
   whitelisted chats.
2. **Asks Funes** via `/api/chat` with the `whatsapp-autoresponder` agent.
   That agent has no send capability -- only `recall`, `remember`, `read_file`.
3. **Sends** the reply back via the bridge's REST API, hardcoded to the exact
   chat the incoming message came from.

Features:
- Per-chat sessions with generation rotation via `/new` command (handled in
  Python, never forwarded to the LLM)
- Document/image attachment handling: downloads, copies to upload directory,
  passes `[Document received: <path>]` markers to the agent
- Expired upload cleanup (default 30 days)
- State persistence in `~/.funes/whatsapp_autoresponder_state.json`
- First-run state seeds from the bridge DB's max timestamp (no backlog replay)

## Service files

| File | Purpose |
|---|---|
| `whatsapp-autoresponder.service` | systemd user service for the autoresponder script |
| `whatsapp-bridge.service` | systemd user service for the personal WhatsApp bridge (port 8090) |
| `whatsapp-bridge-funes.service` | systemd user service for the dedicated Funes WhatsApp bridge (port 8091) |

Two WhatsApp numbers, two bridge instances: the personal number is used by
`whatsapp-assistant` (interactive, delegated from funes), and the dedicated
number is used by the autoresponder (automated, polled by this script).
