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

Per-user permissions apply on top, and can name a single script:
`{"tools": {"run_script": false}}` takes scripts away from an account through
every agent, `{"tools": {"run_script:backup_workspace": false}}` takes away
that one. `run_script` is **not** privileged the way `execute_shell` is,
because a member running an admin-installed, admin-granted script with
declared parameters is a different act from a member running arbitrary code.

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
output:                      # optional; see "Declaring what a script returns"
  format: json
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

## Declaring what a script returns

A script whose result feeds another step should say what that result looks
like, and the runtime checks it:

```yaml
output:
  format: json          # text (default) | json
  schema:               # json only — the answer_schema.h subset
    type: object
    required: [archive, bytes]
    properties:
      archive: {type: string}
      bytes:   {type: number}
```

This is the contract `pipelines/*.yaml` puts on a pipeline stage, applied to a
script — and it exists for the same reason. Exit code 0 is the *script* saying
it worked. Without a declared shape, a script that exits 0 having printed an
object where an array was expected reports success, and the failure surfaces
two steps later in whatever went looking for the array. A mismatch here is a
tool error instead, worded as an installation fault ("this is a fault in the
installed script, not in how it was called") so the model reports it rather
than retrying with different arguments against a script that cannot satisfy
it.

Two rules for a JSON script:

- **stdout is exactly one JSON value.** No banner line, no trailing "done".
  The parse is strict on purpose: being lenient would let a stray debug line
  decide which of two JSON objects the next step reads.
- **stderr is yours.** For a JSON script the two streams are captured
  separately, so warnings, progress and tracebacks on stderr do not corrupt
  the contract — and they are shown alongside the error when the script fails
  or breaks its own shape. (A text script keeps the merged stream: for a
  person reading a run, interleaved is how it happened.)

A JSON script's result comes back as the JSON itself — no `exit_code: 0`
banner in front of it, since the point is that the next step can consume it
(hand it to `write_structured`, read a field out of it) without stripping
anything first. The exit code still decides success; a *failing* JSON script
returns the usual `exit_code: N` plus its stderr, because that is what a
person needs to see.

`list_scripts` and the agent's system prompt both show the declared shape
("returns: JSON object with keys: archive, bytes"), so a model can plan the
next step instead of calling once to find out.

## Budgets, contracts and per-account grants

`run_script` is one tool standing in for n programs, so anything that keys on a
tool name also accepts a **qualified key**, `run_script:<script>`:

```yaml
tool_limits:
  run_script: 6                      # every script, together
  run_script:backup_workspace: 1     # this one, once per run
require_tools: [run_script:publish_issue]   # that script, not merely some script
```

Without the qualified form, three granted scripts would share one ceiling, and
`require_tools: [run_script]` would be satisfied by whichever script happened
to run — a contract that looks enforced and isn't. A budget spent on one
script refuses that script by name and leaves the others callable; only the
aggregate ceiling withdraws the tool.

Permissions take the same key, which is how one account is denied one script
through every agent that has it:

```bash
./bin/funes perms marta --deny run_script:backup_workspace
```

A denied script is left out of `list_scripts` and out of the prompt, so the
model never plans around a capability it would be refused halfway through; the
agent loop re-checks the key at dispatch, which is what catches a call written
out as prose.

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

## Running one on a schedule

`schedule_job(kind="script", script="backup_workspace", arguments={...})`.
Unattended work that used to mean `kind: "shell"` — and therefore
`FUNES_ALLOW_SHELL=1` for the whole install — is a script job now.

The job goes through the same `run_script` path an interactive call takes, so
one set of rules applies to both: the agent's grant, the account's
permissions, the arguments validated against the manifest, the workspace cwd,
the declared output shape. Two things are checked **when the job fires**, not
just when it was written: the scheduling agent still has the script in its
`scripts:` list, and the owner is still permitted to run it. Removing a grant
therefore stops the timer too — a job whose authorization has gone records
`FAILED — the agent 'operator' is no longer granted the script 'x'` and does
nothing.

Arguments are validated at scheduling time as well, so a job whose arguments
were never going to work fails while the person who wrote them is still there,
not at 3am on the first firing.

## The two examples here

| Script | Shows |
|---|---|
| `workspace_report` | `interpreter: python3`, an optional string param and a boolean flag, read-only work. |
| `backup_workspace` | no `interpreter:` (shebang + executable bit), a declared JSON output shape, a side effect that used to require `execute_shell`. |

Both re-validate their own arguments even though Funes validated them first: a
script here should be safe to run on its own terms, not safe only because of
who called it.
