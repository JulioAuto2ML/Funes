// =============================================================================
// src/core/local_peer.h — who is on the other end of funes.sock
// =============================================================================
// The parts of the local socket (src/server/local_socket.h) that decide
// rather than move bytes, kept in the core so they can be tested alone.
// =============================================================================

#pragma once
#include <string>
#include <sys/types.h>

namespace funes {

// FUNES_SOCKET, else $XDG_RUNTIME_DIR/funes.sock; "" when neither is set or
// FUNES_SOCKET is "off". Shared with `funes ask`, so both ends agree.
std::string local_socket_path();

// The cgroup a process is in, as /proc/<pid>/cgroup's v2 line names it.
std::string cgroup_of(pid_t pid, const std::string& proc_root = "/proc");

// The server's unit cgroup: its own, with the "/server" leaf the cgroup
// module (cgroup.h) moves it into stripped — the parent of every exec-*.
// "" when the server is not in such a leaf (FUNES_CGROUPS off): its cgroup
// is then shared with whatever started it and proves nothing.
std::string service_cgroup_of(pid_t self, const std::string& proc_root = "/proc");

// True if `peer` is Funes's own child: inside `service_cgroup`, or with
// `self` among its ancestors. A peer with no pid counts as one (refused).
bool is_own_child(pid_t peer, pid_t self, const std::string& service_cgroup,
                  const std::string& proc_root = "/proc");

// The message a run gets: the question, then the piped input fenced and
// labelled as data — its last `max_input` bytes, since the end of a log is
// usually where it broke.
std::string frame_local_message(const std::string& question, const std::string& input,
                                size_t max_input);

} // namespace funes
