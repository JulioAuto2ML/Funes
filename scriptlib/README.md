# scriptlib/ — this extension's scripts for `run_script`

Loaded beside the core's library: `FUNES_SCRIPTS_DIR=<Funes>/scriptlib:<here>/scriptlib`.
Same rules as the core's (see `Funes/scriptlib/README.md`): one manifest plus one
executable per script, granted per agent under `scripts:`.

| Script | Agent | What |
|---|---|---|
| `tv_art` | `home` | Samsung Frame Art Mode: `status`, `show --image=<workspace path>`, `artmode_on`/`off`. JSON out. |

## Setup (once per machine)

```bash
python3 -m venv scriptlib/.venv
scriptlib/.venv/bin/pip install -r scriptlib/requirements.txt
```

`tv_art.sh` runs `tv_art.py` with that venv and exits 3 with the command above
if it is missing. Then set `FUNES_TV_HOST` and `FUNES_TV_TOKEN_FILE` in Funes's
`config/funes.local` (see `config/funes.conf.example`) and accept the "Allow?"
prompt on the TV the first time.

## tv_art — what it decides for itself

- **The image path.** A script runs outside `fs_guard`, so `--image` is checked
  here: relative, inside the working directory (the caller's workspace), symlinks
  resolved, `.jpg`/`.jpeg`/`.png`, at most 20 MB.
- **Whether a change happened.** The set routinely applies a command and then
  times out answering it. Every change is read back; the JSON reports the state
  read back, not the reply.
- **What it deletes.** `show` deletes the previous image *it* uploaded (recorded
  in `<token file>.state.json`) unless `--keep`. It never deletes an id it did
  not upload, so the Art Store pieces and the owner's own photos are safe.

Tests: `python3 -m unittest discover -s scriptlib/tests` (the TV is stubbed).
