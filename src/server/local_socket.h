// =============================================================================
// src/server/local_socket.h — funes.sock, where the kernel says who is asking
// =============================================================================
// OS plan, phase 3 (7.8). The web UI needs a password and the WhatsApp
// autoresponder a service token; a program on this machine needs neither.
// A Unix socket ($XDG_RUNTIME_DIR/funes.sock, mode 0600) hands the server the
// peer's uid with SO_PEERCRED — vouched for by the kernel, nothing to leak —
// and uid_users (users.h) says which account that is. Unmapped uids are
// refused, the one Funes runs as included: an admin maps it on purpose
// (`funes uid-map julio julio`).
//
// One hole that has to be closed by hand: a model-driven child (a script, a
// shell command, an MCP server) runs as the same uid, and Landlock up to ABI 6
// does not gate connect() on a pathname socket. Mapped to an admin, the socket
// would hand a member's sandboxed script the admin's account. So a peer that
// is one of Funes's own children is refused — by cgroup (with FUNES_CGROUPS
// every child lives under the service's cgroup) and by ancestry (without).
//
// The protocol is one JSON line in and JSON lines out, which is all `funes
// ask` needs:
//   → {"message": "...", "input": "...", "agent": "...", "session": "..."}
//   ← {"type": "delta", "text": "..."}            the answer as it is written
//   ← {"type": "tool_call", "name": "..."}
//   ← {"type": "done", "text": "..."} | {"type": "error", "message": "..."}
// `input` is what was piped into `funes ask`: framed in the message as data,
// not instructions, it makes the run's provenance tainted and the run write
// no auto-memory (Persist::TurnsOnly) — a log is not something you said.
// =============================================================================

#pragma once
#include "local_peer.h"
#include <string>

class FunesApi;
class UserStore;

namespace funes {

// Binds `path` and serves it on background threads. False (and a reason on
// stderr) if it cannot — the server runs without the socket, not without
// starting.
bool start_local_socket(const std::string& path, FunesApi& api, UserStore& users);

// `funes ask [--agent A] [--session S] [--max-input N] [-v] [question ...]`:
// the socket's client. Reads stdin when it is not a terminal. Exit 0 on an
// answer, 1 on a failed run, 2 on usage, 3 when the server refused or is not
// there.
int ask_main(int argc, char** argv);

} // namespace funes
