# Bulk Upload

**Status:** MVP done (steps 1–2)
**Effort:** T2 (2–3 days)
**Motivation:** uploading a project's worth of files (a book manuscript, a case archive, a client folder) one at a time through the paperclip button is painful enough that most users won't do it. This is the single biggest friction point for any vertical that works with existing documents.

---

## What it does

A user selects multiple files — or a single ZIP — and they all land in a named subdirectory of their workspace, available to `read_file` in every future conversation.

The key difference from the current upload flow: **bulk-uploaded files are workspace files, not conversation attachments.** Their content is not inlined into the chat message. Instead, Funes confirms what was saved and where, and the user (or an agent) uses `read_file` to access them as needed.

---

## Current state

- The `<input type="file" multiple>` in the UI already accepts multiple files and loops through them sequentially (`app.js:911–914`).
- Each file is POSTed individually to `POST /api/upload`, which saves it to the user's workspace with a timestamp prefix and returns the text/image content for inlining.
- `cpp-httplib` supports `get_file_values` (plural) for multiple files in one multipart request.
- There is no ZIP library in the project. The only compression dependency is zlib (transitive, through SQLite).

---

## Design

### API: `POST /api/upload-batch`

New endpoint, separate from `/api/upload` so the existing single-file flow doesn't change.

**Request:** `multipart/form-data` with:
- One or more `file` fields (each a file).
- One optional `folder` field (string) — the subdirectory name inside the workspace. Default: derived from the first file's name or "uploads".
- One optional `unzip` field (boolean, default true) — if a file is a `.zip`, extract its contents into the folder instead of saving the archive.

**Response:**
```json
{
  "ok": true,
  "folder": "book",
  "files": [
    {"filename": "chapter1.md", "size": 12400, "is_text": true},
    {"filename": "style-sheet.pdf", "size": 1537172, "is_text": true},
    {"filename": "diagram.png", "size": 64755, "is_text": false, "is_image": true}
  ],
  "skipped": [
    {"filename": "huge.pdf", "reason": "exceeds 5 MB limit"}
  ]
}
```

No file content is returned in the response. Files are saved to disk only.

**Size limits:**
- Per-file: keep the existing 5 MB limit.
- Per-request: 50 MB total (prevents a 200-file ZIP from exhausting disk).
- File count: 100 files max (after ZIP extraction).

**Filename handling:**
- No timestamp prefix. The point is human-readable filenames so `read_file book/chapter1.md` works naturally.
- If a file with the same name exists in the target folder, overwrite it (the user is updating their material).
- Sanitize: strip path components (no `../../`), replace spaces with underscores, reject names that start with `.`.

### ZIP extraction

Rather than adding a library dependency, shell out to `unzip` — it's present on every Linux system and Funes already shells out to `pdftotext` and `pdftoppm` for PDF handling. The same pattern applies:

```cpp
// Pseudocode
std::string cmd = "unzip -o -j " + shell_escape(zip_path)
                + " -d " + shell_escape(dest_dir);
auto [ok, output] = run_with_timeout(cmd, 30s);
```

`-j` flattens the directory structure (no nested folders — keeps `read_file` paths simple). `-o` overwrites existing files.

After extraction, delete the ZIP file itself from the workspace.

Nested ZIPs are not extracted — they're saved as-is.

### UI: folder upload mode

Two options, and the simpler one is probably right for now:

**Option A (minimal):** add a "Upload folder" button next to the existing paperclip. Uses `<input type="file" webkitdirectory>` to let the user select a folder. All files are POSTed to `/api/upload-batch` with the folder name derived from the selected directory. Browser support: Chrome, Edge, Firefox (not Safari on iOS, but Funes is a desktop tool).

**Option B (also minimal):** keep the existing paperclip, but when the user selects more than 3 files, switch from the current per-file `/api/upload` loop to a single `/api/upload-batch` call and prompt for a folder name.

Recommendation: **Option A**. A separate button makes the intent clear and avoids surprising behavior changes on the existing flow. The button appears next to the paperclip:

```
📎  📁
```

The 📁 button opens the folder picker. After selection, a confirmation strip shows: "Upload 27 files to `book/`? [Upload] [Cancel]" — with the folder name editable.

### Confirmation message

After a successful batch upload, insert a system-style message into the chat:

> 📁 Uploaded 27 files to `book/`
> chapter1.md, chapter2.md, chapter3.md, ... and 24 more.
> Use `read_file book/<filename>` to access them.

This is a local UI message, not a model turn — it costs no tokens.

### fs_guard: no changes needed

The target folder is created inside the user's workspace via `workspace_for(root, user_id, "")` + the folder name, then each file is saved there. `resolve()` already accepts relative paths and creates intermediate directories. No confinement rules change.

---

## Implementation order

1. ~~**Server: `/api/upload-batch` endpoint**~~ — done. Accepts multiple files, saves to a named subfolder, returns the manifest. Integration tests added.
2. ~~**UI: folder upload button**~~ — done. `webkitdirectory` input with confirmation strip (editable folder name), POST to the new endpoint, success chip in chat.
3. **Server: ZIP extraction** — detect `.zip` files in the batch, shell out to `unzip`, flatten into the target folder.
4. **UI: drag-and-drop a ZIP** — when a `.zip` is dropped (even on the existing paperclip), route it to `/api/upload-batch` with `unzip=true`.

Steps 1–2 are done. Steps 3–4 are a follow-up.

---

## What this unblocks

- **Book writing:** upload the entire `Book writing/` folder once, then `read_file book/chapter1.md` in any conversation.
- **Law firms:** upload a case folder (contracts, correspondence, exhibits) and have an agent review them.
- **Therapists:** upload intake forms and session notes for a client.
- **Any project-based work:** the pattern is always "here are my files, now help me work with them."

---

## Out of scope (for now)

- **Folder structure inside the workspace.** Extracted ZIPs are flattened. If a user needs subdirectories, they can run multiple batch uploads with different folder names.
- **A file browser UI.** This plan gets files in; browsing and managing them is a separate feature.
- **`list_files` tool.** Closely related but independent — an agent tool that lists the contents of a workspace directory. Worth building alongside this but tracked separately.
- **Syncing.** This is a one-time upload, not a sync. If the user updates a file locally, they re-upload it.
