# scripts/

Operational scripts and systemd service files for running Funes and its
companion services.

## Scripts

| Script | Purpose |
|---|---|
| `funesctl.sh` | Thin wrapper around `systemctl --user start/stop/restart/status funes.service`. |
| `funes_bench.py` | Tool-calling benchmark: drives `/api/chat` through a fixed suite of prompts and scores whether the currently-connected model calls the right tools (and avoids the wrong ones). Run after swapping `FUNES_LLM_MODEL` to check the new model still works with Funes' agents before trusting it. `--list` for cases, `--compare` to diff saved runs across models. |
| `whatsapp_autoresponder.py` | Polls the WhatsApp bridge for incoming messages, asks the `whatsapp-autoresponder` agent for a reply, sends it back. |
| `whatsapp_whitelist.py` | CLI tool for managing the autoresponder contact whitelist (`list/add/remove`). |

## WhatsApp autoresponder

A standalone Python service (stdlib only, no external dependencies) that runs
as a systemd user service. The safety model is that the LLM never chooses a
recipient:

1. **Polls** the WhatsApp bridge's SQLite database for new messages from
   whitelisted chats.
2. **Asks Funes** via `/api/chat` with the `whatsapp-autoresponder` agent,
   authenticating with `FUNES_SERVICE_TOKEN` plus the sender's jid. That agent
   has no send capability -- only `recall`, `remember`, `read_file`.
3. **Sends** the reply back via the bridge's REST API, hardcoded to the exact
   chat the incoming message came from.

Since 4.0 the number must also be mapped to a Funes account
(`funes jid-map <jid> <username>`) or the API refuses the call. Whitelisting
decides *whether* to reply; the mapping decides *whose* memories and files the
reply is drawn from. Both are required.

Features:
- Per-chat sessions with generation rotation via `/new` command (handled in
  Python, never forwarded to the LLM)
- Document/image attachment handling: downloads, copies into the sending
  contact's own Funes workspace (`<workspace>/<user_id>/whatsapp-uploads/`,
  the account resolved by asking Funes rather than guessed locally), passes
  `[Document received: <path>]` markers to the agent
- Expired upload cleanup (default 30 days), sweeping every account's upload
  folder rather than one shared directory
- State persistence in `~/.funes/whatsapp_autoresponder_state.json`, or
  `WHATSAPP_STATE_PATH` if set. Two installs on one box share one bridge
  store, so a second poller must have its own watermark — otherwise the
  symptom is not an error, it is one poller silently skipping messages the
  other consumed
- First-run state seeds from the bridge DB's max timestamp (no backlog replay)

## Service files

| File | Purpose |
|---|---|
| `whatsapp-autoresponder-v4.service` | the autoresponder script against the 4.0 install on :8485. The 3.x counterpart was removed on 2026-08-31 when 3.x was retired, so there is no longer a second unit to conflict with. Needs `FUNES_SERVICE_TOKEN`, `WHATSAPP_WHITELIST`, `FUNES_API_URL`, `WHATSAPP_STATE_PATH` and `WHATSAPP_DB_PATH` in the v4 clone's `config/funes.local`, plus a `funes jid-map` per sender |
| `whatsapp-bridge.service` | systemd user service for the personal WhatsApp bridge (port 8090). Runs from `~/whatsapp-bridge/`, deliberately outside either clone |
| `whatsapp-bridge-funes.service` | systemd user service for the dedicated Funes WhatsApp bridge (port 8091). Same directory, `WHATSAPP_STORE_DIR=store-funes` |
| `funes-v4.service` | systemd user service for Funes on yoda — since 3.x was retired on 2026-08-31 this is the only install. Every path and port is still set explicitly so a config file can never point it at the dormant 3.x database. See [docs/deploy-v4-yoda.md](../docs/deploy-v4-yoda.md) |

Two WhatsApp numbers, two bridge instances: the personal number is used by
`whatsapp-assistant` (interactive, delegated from funes), and the dedicated
number is used by the autoresponder (automated, polled by this script).

### Authenticating the poller against 4.0

4.0 answers 401 to an unauthenticated `/api/chat`, so the script needs a
`FUNES_SERVICE_TOKEN` — and the token alone is not enough. It says the caller
is trusted; it does not say who the message is *for*. The script sends the
sender's `chat_jid` alongside it, and Funes maps that jid to an account:

```bash
cd ~/Funes-v4 && FUNES_DB=~/.funes-v4/memory.db \
    ./bin/funes jid-map '<chat_jid>' <username>
```

An unmapped jid resolves to nobody, never to a default account — so adding a
number to `WHATSAPP_WHITELIST` without mapping it gets a refusal, not somebody
else's memories. `funes jid-unmap` reverses it. Neither the token nor the jid
authenticates anything on its own.

Two paths have to be overridden for a second install, and both fail in ways
that do not look like configuration:

- **`WHATSAPP_STATE_PATH`** — the read-watermark. Shared between two pollers,
  one silently skips messages the other consumed. No error is logged.
- **`WHATSAPP_DB_PATH`** — the autoresponder reads the *dedicated* bridge
  (`store-funes`, :8091), not the personal one, and `funes.conf` gives that
  path relative to the repo root. A second clone has no `store-funes`, so the
  poller crash-loops on "unable to open database file" until it is set to an
  absolute path. Both bridges still run out of the original clone; moving
  their stores somewhere install-independent is the outstanding fix.
