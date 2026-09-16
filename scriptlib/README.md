# scriptlib/ — the script library

One manifest plus one executable file per script. This directory is the only
place `run_script` will start anything from, and an agent reaches a script in
it only if its own YAML names that script under `scripts:`.

## Why this exists

An agent that needed to run one known program used to need `execute_shell`,
which is a command line the model writes, run with the Funes process's own
permissions, behind one global switch (`FUNES_ALLOW_SHELL=1`). "Let this agent
take the nightly backup" and "let this agent run anything at all" were the
same grant. They are not the same request.

A script is declared the way a tool is: named in the agent's allowlist,
described by a manifest, called with typed parameters. The model supplies
**arguments, never a command**, and they reach the process as argv tokens —
no shell parses them, so a value containing `;` or `$(…)` is one argument with
punctuation in it. So the narrow thing an agent actually needs can be granted
without the broad thing it doesn't:

| | `execute_shell` | `run_script` |
|---|---|---|
| What the model chooses | the command | which of *n* named scripts, and its arguments |
| Parsed by a shell | yes | no — argv straight to `execvp` |
| Global switch | `FUNES_ALLOW_SHELL=1` | none: the grant *is* the control |
| Granted by | agent `tools:` + user permissions | the same, **plus** agent `scripts:` |
| Adding a capability | nothing to add — it can already run anything | an admin installs a file here |

What this is **not** is a sandbox. A script runs with the Funes process's own
permissions and can do anything that account can. The guarantee is about which
code can be started, and by whom — the same kind of trust `agents/*.yaml`
already carries. Review a script before installing it exactly as you would
review an agent prompt, and do not install one that takes a command, a path
outside the workspace, or a URL to fetch-and-run: that hands back everything
the split was for.

## Where it lives, and who can reach it

`FUNES_SCRIPTS_DIR`, default `./scriptlib`. Deliberately outside every
workspace: `read_file`, `write_file` and `/api/upload` are confined to
`<workspace>/<user_id>/` by `fs_guard`, so nothing the model drives can read a
script, edit one, or add one. Installing a script is an admin editing the
repo.

A script runs with the **calling account's workspace as its working
directory** — the same directory `read_file`/`write_file` resolve to, from the
same `fs_guard::workspace_for`. Write relative paths and one account's run
cannot land in another's folder.

## Granting a script to an agent

```yaml
# agents/operator.yaml
tools:   [read_file, write_file, list_scripts, run_script, ...]
scripts: [workspace_report, backup_workspace]
```

Both halves are required: `run_script` in `tools:` (so the agent has the tool
at all) and the script's name in `scripts:` (so it may run *that* script).

**`scripts:` denies by default.** An absent or empty list means *no scripts* —
the opposite of `tools:`, where empty means everything registered. A tool is
code compiled into the binary; a script is a file that appears in a directory,
and "every agent may run every file that appears here" is not a default anyone
would choose on purpose. Each grant is written down.

Per-user permissions still apply on top: `run_script` is an ordinary tool, so
an admin can deny it to an account (`{"tools": {"run_script": false}}`) and
that account loses scripts through every agent. It is **not** privileged the
way `execute_shell` is, because a member running an admin-installed,
admin-granted script with declared parameters is a different act from a member
running arbitrary code.

A generated agent gets nothing here either: `create_agent` writes no
`scripts:` key and has no argument for one, so granting a script is always a
person editing a YAML. An agent that can mint agents must not be able to mint
itself the grant.

## Manifest format

`<name>.yaml`, where `<name>` — the filename stem — is what the model calls
and must be `[a-z0-9_-]`. A `name:` field inside the file is ignored: the
filename is the name, so the two can never disagree.

```yaml
description: One line. This is what the model sees when it decides to run it.
run: backup_workspace.sh     # a filename in this directory — not a path
interpreter: python3         # optional; omit to exec the file itself (shebang + chmod +x)
timeout_seconds: 120         # default 30, ceiling 120 — killed as a process group
params:
  - name: path
    description: Shown to the model alongside the script.
    type: string             # string | number | boolean
    required: false
    choices: [daily, weekly] # optional; string params only
env:
  API_KEY: "${SOME_SERVICE_KEY}"   # expanded from the server environment at load
```

`run:` must be a plain filename inside this directory — no `/`, no `..`. A
manifest that could point at `/usr/bin/anything` would make the grant
meaningless, since the claim is that an allowed script is a reviewed file in
one reviewed directory.

`env:` is how a script gets a credential: the value is expanded from the
server's environment (`config/funes.local`) when the manifest loads and handed
to the child process. The model names the script; it never sees the key, and
the key never enters the transcript.

## How arguments arrive

Declared parameters are passed as **`--name=value`** tokens, in manifest
order, after the script path. A `true` boolean is a bare `--name`; a `false`
or omitted one is simply absent.

```
run_script(name="backup_workspace", arguments={"name": "before-upgrade"})
  → execvp: ["/…/scriptlib/backup_workspace.sh", "--name=before-upgrade"]
```

One token rather than two (`--name` `value`) on purpose: `--name=value` can
never be read as a flag in its own right, whatever the value starts with,
so a value of `--force` arrives as data and not as an option the script
recognises.

Before anything starts, Funes refuses: a parameter the manifest doesn't
declare, a missing required one, a wrong type, a value outside `choices`, a
value over 4 KB, and any value containing a control character. The refusal
names the accepted parameters, so the model's next attempt is a corrected call
rather than a guess.

Parsing the tokens is three lines in either language:

```python
key, sep, value = token[2:].partition("=")   # python
```
```bash
case "$token" in --name=*) v="${token#--name=}" ;; esac   # bash
```

## Output and failure

stdout and stderr are combined, capped at 16 KB, and returned as
`exit_code: N` followed by the output — the same shape `execute_shell`
returns, so a prompt that already reads one reads the other. A non-zero exit
or a timeout is a tool *error*, not content. Exit 127 is reported separately
as an installation fault (missing interpreter, or the file is not executable),
because it is not something the arguments can fix and a model told only
"failed" will otherwise keep re-calling with different arguments.

Output over 2 KB goes through the result store like any other tool result: the
model sees a head/tail preview and a `result_id` to dereference, so a chatty
script cannot eat the context window.

## What this does not do yet

`schedule_job` still has two kinds, `agent` and `shell`; there is no
`kind: script`, so an unattended run of a library script goes through the
shell kind and therefore still needs `FUNES_ALLOW_SHELL=1`. The natural next
step, and not done here.

Nothing enforces a per-*user* script grant either: `run_script` is one ordinary
tool, so an admin can deny it to an account wholesale but cannot grant one
account two scripts and another account three through the same agent. Split the
agent if you need that.

## The two examples here

| Script | Shows |
|---|---|
| `workspace_report` | `interpreter: python3`, an optional string param and a boolean flag, read-only work. |
| `backup_workspace` | no `interpreter:` (shebang + executable bit), a side effect that used to require `execute_shell`. |

Both re-validate their own arguments even though Funes validated them first: a
script here should be safe to run on its own terms, not safe only because of
who called it.
