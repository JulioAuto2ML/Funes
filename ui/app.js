/* =============================================================================
   Funes UI — chat + memory browser (no build step, no dependencies)
   ========================================================================== */

'use strict';

const $ = (id) => document.getElementById(id);

const els = {
  messages:     $('messages'),
  welcome:      $('welcome'),
  composer:     $('composer'),
  input:        $('input'),
  send:         $('send'),
  newChat:      $('new-chat'),
  toggleChats:  $('toggle-chats'),
  chatsPane:    $('chats-pane'),
  chatsList:    $('chats-list'),
  toggleMemory: $('toggle-memory'),
  memoryPane:   $('memory-pane'),
  memoryList:   $('memory-list'),
  toggleJobs:   $('toggle-jobs'),
  jobsPane:     $('jobs-pane'),
  jobsList:     $('jobs-list'),
  memorySearch: $('memory-search'),
  memoryAdd:    $('memory-add'),
  memoryAddText:$('memory-add-text'),
  memoryCount:  $('memory-count'),
  semanticBadge:$('semantic-badge'),
  statusDot:    $('status-dot'),
  contextGauge: $('context-gauge'),
  contextBarFill:$('context-bar-fill'),
  contextPct:   $('context-pct'),
  attachments:  $('attachments'),
  attachBtn:    $('attach-btn'),
  fileInput:    $('file-input'),
};

const urlSession = new URLSearchParams(location.search).get('session');

const state = {
  session: urlSession || localStorage.getItem('funes.session') || newSessionId(),
  busy:    false,
  abortCtrl: null,
  // Text: {kind:'text', filename, content, isText, truncated}
  // Image: {kind:'image', filename, mimeType, data (base64)}
  attachments: [],
};
localStorage.setItem('funes.session', state.session);

function newSessionId() {
  return 's-' + Date.now().toString(36) + '-' +
         Math.random().toString(36).slice(2, 10);
}

/* ── tiny markdown (escape first, then decorate) ─────────────────────────── */

function escapeHtml(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

function renderMarkdown(text) {
  let html = escapeHtml(text);
  // fenced code blocks
  html = html.replace(/```([\s\S]*?)```/g,
    (_, code) => '<pre><code>' + code.replace(/^\w*\n/, '') + '</code></pre>');
  html = html.replace(/`([^`\n]+)`/g, '<code>$1</code>');
  html = html.replace(/\*\*([^*\n]+)\*\*/g, '<strong>$1</strong>');
  html = html.replace(/(^|\s)\*([^*\n]+)\*(?=\s|[.,;:!?]|$)/g, '$1<em>$2</em>');
  html = html.replace(/\[([^\]\n]+)\]\((https?:\/\/[^)\s]+)\)/g,
    '<a href="$2" target="_blank" rel="noopener noreferrer">$1</a>');
  return html;
}

/* ── chat rendering ──────────────────────────────────────────────────────── */

function hideWelcome() {
  if (els.welcome) { els.welcome.remove(); els.welcome = null; }
}

function scrollDown() {
  els.messages.scrollTop = els.messages.scrollHeight;
}

function addMessage(role, text, images) {
  hideWelcome();
  const div = document.createElement('div');
  div.className = 'msg ' + role;
  if (images && images.length) {
    const thumbs = document.createElement('div');
    thumbs.className = 'msg-images';
    for (const img of images) {
      const el = document.createElement('img');
      el.src = 'data:' + img.mimeType + ';base64,' + img.data;
      el.alt = img.filename || 'attached image';
      thumbs.appendChild(el);
    }
    div.appendChild(thumbs);
  }
  const textEl = document.createElement('div');
  if (role === 'assistant') textEl.innerHTML = renderMarkdown(text);
  else textEl.textContent = text;
  if (text) div.appendChild(textEl);
  els.messages.appendChild(div);
  scrollDown();
  return div;
}

function addChip(cls, icon, label, detail) {
  hideWelcome();
  const wrap = document.createElement('div');
  wrap.className = 'activity';
  const chip = document.createElement('div');
  chip.className = 'chip ' + cls;
  chip.innerHTML = '<span class="chip-icon">' + icon + '</span>' + escapeHtml(label);
  if (detail) {
    const d = document.createElement('div');
    d.className = 'detail';
    d.textContent = detail;
    chip.appendChild(d);
    chip.addEventListener('click', () => chip.classList.toggle('open'));
    chip.style.cursor = 'pointer';
  }
  wrap.appendChild(chip);
  els.messages.appendChild(wrap);
  scrollDown();
  return chip;
}

/* ── SSE chat ────────────────────────────────────────────────────────────── */

async function sendMessage(displayText, fullText, images) {
  state.busy = true;
  state.abortCtrl = new AbortController();
  els.send.disabled = false;
  els.send.textContent = '■';
  els.send.classList.add('stop');
  els.send.title = 'Stop';
  addMessage('user', displayText,
    images.map(img => ({ mimeType: img.mime_type, data: img.data })));

  const bubble = addMessage('assistant', '');
  bubble.classList.add('thinking');
  let streamed = '';

  try {
    const resp = await fetch('/api/chat', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      signal: state.abortCtrl.signal,
      body: JSON.stringify({
        message: fullText,
        images:  images,
        session: state.session,
      }),
    });

    if (!resp.ok) {
      const err = await resp.json().catch(() => ({}));
      throw new Error(err.error || ('HTTP ' + resp.status));
    }

    const reader = resp.body.getReader();
    const decoder = new TextDecoder();
    let buffer = '';

    const handleEvent = (type, data) => {
      if (type === 'delta') {
        streamed += data.text;
        bubble.innerHTML = renderMarkdown(streamed);
        scrollDown();
      } else if (type === 'memories') {
        const n = data.items.length;
        const detail = data.items.map(m => '• ' + m.text).join('\n');
        els.messages.insertBefore(
          bubbleChipBefore('memory', '📖',
            'Funes remembered ' + n + (n === 1 ? ' thing' : ' things'), detail),
          bubble.parentNode === els.messages ? bubble : null);
        scrollDown();
      } else if (type === 'tool_call') {
        els.messages.insertBefore(
          bubbleChipBefore('', '⚙️', data.name + '(' + summarizeArgs(data.args) + ')', null),
          bubble);
        scrollDown();
      } else if (type === 'tool_result') {
        // fold result into the last matching tool chip as expandable detail
        const chips = els.messages.querySelectorAll('.chip:not(.memory)');
        const last = chips[chips.length - 1];
        if (last && !last.querySelector('.detail')) {
          const d = document.createElement('div');
          d.className = 'detail';
          d.textContent = data.preview;
          last.appendChild(d);
          last.style.cursor = 'pointer';
          last.addEventListener('click', () => last.classList.toggle('open'));
          if (data.error) last.classList.add('error');
        }
      } else if (type === 'result_stored') {
        // A big tool result was kept out of the transcript (see
        // core/result_store.h). Worth surfacing: the agent's next move is
        // usually a read_result call, which would otherwise look unmotivated.
        const kb = data.bytes >= 1024 ? Math.round(data.bytes / 1024) + ' KB'
                                      : data.bytes + ' bytes';
        els.messages.insertBefore(
          bubbleChipBefore('compress', '📦',
            'Stored ' + kb + ' from ' + data.tool + ' (result #' + data.result_id + ')',
            null),
          bubble);
        scrollDown();
      } else if (type === 'contract_nudge') {
        els.messages.insertBefore(
          bubbleChipBefore('', '↩️',
            'Answered early — still owes ' + (data.missing || []).join(', '), null),
          bubble);
        scrollDown();
      } else if (type === 'schema_nudge') {
        els.messages.insertBefore(
          bubbleChipBefore('', '↩️', 'Answer rejected: ' + data.error, null),
          bubble);
        scrollDown();
      } else if (type === 'context_compressed') {
        const n = data.turns_folded;
        els.messages.insertBefore(
          bubbleChipBefore('compress', '🗜️',
            'Compressed ' + n + (n === 1 ? ' turn' : ' turns') + ' into a summary',
            data.summary_preview),
          bubble);
        scrollDown();
      } else if (type === 'usage') {
        updateUsage(data);
      } else if (type === 'done') {
        bubble.innerHTML = renderMarkdown(data.text || streamed || '…');
      } else if (type === 'error') {
        bubble.classList.remove('thinking');
        bubble.innerHTML = renderMarkdown('⚠️ ' + data.message);
      }
    };

    let eventType = 'message';
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });

      let idx;
      while ((idx = buffer.indexOf('\n')) >= 0) {
        const line = buffer.slice(0, idx).replace(/\r$/, '');
        buffer = buffer.slice(idx + 1);
        if (line.startsWith('event:')) {
          eventType = line.slice(6).trim();
        } else if (line.startsWith('data:')) {
          try { handleEvent(eventType, JSON.parse(line.slice(5))); }
          catch (e) { /* partial frame — ignore */ }
        }
      }
    }
  } catch (e) {
    if (e.name === 'AbortError') {
      if (!streamed) bubble.innerHTML = renderMarkdown('*(stopped)*');
    } else {
      bubble.innerHTML = renderMarkdown('⚠️ ' + e.message);
    }
  } finally {
    bubble.classList.remove('thinking');
    state.busy = false;
    state.abortCtrl = null;
    els.send.disabled = false;
    els.send.textContent = '↑';
    els.send.classList.remove('stop');
    els.send.title = 'Send';
    els.input.focus();
    refreshMemories();
    refreshJobs();
    if (!els.chatsPane.hidden) refreshChats();
  }
}

// Build a chip element (not appended) so it can be inserted before the bubble.
function bubbleChipBefore(cls, icon, label, detail) {
  const wrap = document.createElement('div');
  wrap.className = 'activity';
  const chip = document.createElement('div');
  chip.className = 'chip ' + cls;
  chip.innerHTML = '<span class="chip-icon">' + icon + '</span>' + escapeHtml(label);
  if (detail) {
    const d = document.createElement('div');
    d.className = 'detail';
    d.textContent = detail;
    chip.appendChild(d);
    chip.addEventListener('click', () => chip.classList.toggle('open'));
  }
  wrap.appendChild(chip);
  return wrap;
}

function summarizeArgs(args) {
  if (!args || typeof args !== 'object') return '';
  const vals = Object.values(args).map(v =>
    typeof v === 'string' ? (v.length > 40 ? v.slice(0, 40) + '…' : v) : JSON.stringify(v));
  return vals.join(', ');
}

/* ── attachments ─────────────────────────────────────────────────────────── */

async function uploadFile(file) {
  const fd = new FormData();
  fd.append('file', file);
  try {
    const resp = await fetch('/api/upload', { method: 'POST', body: fd });
    const data = await resp.json();
    if (!data.ok) {
      addChip('error', '⚠️', 'Upload failed: ' + (data.error || 'unknown error'));
      return;
    }
    if (data.is_image) {
      state.attachments.push({
        kind: 'image',
        filename: data.filename,
        mimeType: data.mime_type,
        data: data.data,
      });
    } else {
      state.attachments.push({
        kind: 'text',
        filename: data.filename,
        content:  data.content,
        isText:   data.is_text,
        truncated: data.truncated,
      });
    }
    renderAttachments();
  } catch (e) {
    addChip('error', '⚠️', 'Upload failed: ' + e.message);
  }
}

function renderAttachments() {
  els.attachments.innerHTML = '';
  els.attachments.hidden = state.attachments.length === 0;
  state.attachments.forEach((a, i) => {
    const chip = document.createElement('span');
    if (a.kind === 'image') {
      chip.className = 'attachment-chip image';
      const thumb = document.createElement('img');
      thumb.src = 'data:' + a.mimeType + ';base64,' + a.data;
      thumb.alt = a.filename;
      chip.appendChild(thumb);
    } else {
      chip.className = 'attachment-chip' + (a.isText ? '' : ' binary');
      const label = document.createElement('span');
      label.textContent = '📎 ' + a.filename + (a.truncated ? ' (truncated)' : '')
                         + (a.isText ? '' : ' (binary, saved only)');
      chip.appendChild(label);
    }
    const rm = document.createElement('button');
    rm.type = 'button';
    rm.textContent = '✕';
    rm.title = 'Remove attachment';
    rm.addEventListener('click', () => {
      state.attachments.splice(i, 1);
      renderAttachments();
    });
    chip.appendChild(rm);
    els.attachments.appendChild(chip);
  });
}

// What the user sees in their own chat bubble — just the filenames, not the
// (possibly huge) file content that actually gets sent.
function buildDisplayText(text, attachments) {
  const chips = attachments.map(a => '📎 ' + a.filename).join('  ');
  if (chips && text) return chips + '\n' + text;
  return chips || text;
}

// What actually goes to the model as text: file contents inlined as fenced
// blocks, plus a marker line for each image (the pixels go separately, in
// the `images` field — see collectImages — but this marker is what survives
// into future turns, since images themselves aren't persisted in history).
function buildFullText(text, attachments) {
  let out = '';
  for (const a of attachments) {
    if (a.kind === 'image') {
      out += '[Attached image: ' + a.filename + ']\n\n';
      continue;
    }
    out += '[Attached file: ' + a.filename + ']\n';
    out += a.isText ? ('```\n' + a.content + '\n```\n\n')
                    : '(binary file, saved to the workspace, not shown here)\n\n';
  }
  out += text;
  return out.trim() || '(see attached file)';
}

// What goes to the model as actual pixels — only image attachments, in the
// {mime_type, data} shape /api/chat expects.
function collectImages(attachments) {
  return attachments
    .filter(a => a.kind === 'image')
    .map(a => ({ mime_type: a.mimeType, data: a.data }));
}

/* ── context usage gauge ─────────────────────────────────────────────────── */

function updateUsage(data) {
  const pct = data.limit > 0 ? Math.min(100, Math.round((data.used / data.limit) * 100)) : 0;
  els.contextGauge.hidden = false;
  els.contextBarFill.style.width = pct + '%';
  els.contextPct.textContent = pct + '%';
  els.contextGauge.classList.remove('ok', 'warn', 'danger');
  els.contextGauge.classList.add(pct < 60 ? 'ok' : pct < 85 ? 'warn' : 'danger');
  els.contextGauge.title = (data.estimated ? '~' : '') + data.used + ' / ' + data.limit +
                          ' tokens' + (data.estimated ? ' (estimated)' : '');
}

/* ── memory pane ─────────────────────────────────────────────────────────── */

async function refreshMemories() {
  const q = els.memorySearch.value.trim();
  const params = new URLSearchParams();
  if (q) params.set('q', q);
  try {
    const resp = await fetch('/api/memories?' + params);
    const data = await resp.json();
    if (!data.ok) return;

    els.memoryCount.textContent = data.total;
    els.semanticBadge.hidden = !data.semantic;

    els.memoryList.innerHTML = '';
    if (data.memories.length === 0) {
      const empty = document.createElement('div');
      empty.className = 'memory-empty';
      empty.textContent = q ? 'Nothing matches that search.'
                            : 'Funes has no memories yet. Tell it something!';
      els.memoryList.appendChild(empty);
      return;
    }

    for (const m of data.memories) {
      const item = document.createElement('div');
      item.className = 'memory-item';

      const text = document.createElement('div');
      text.textContent = m.text;
      item.appendChild(text);

      const meta = document.createElement('div');
      meta.className = 'meta';
      const src = document.createElement('span');
      src.className = 'src ' + m.source;
      src.textContent = m.source;
      meta.appendChild(src);
      const date = document.createElement('span');
      date.textContent = (m.created_at || '').slice(0, 16);
      meta.appendChild(date);
      if (q && m.score > 0) {
        const score = document.createElement('span');
        score.textContent = (m.score * 100).toFixed(0) + '% match';
        meta.appendChild(score);
      }
      item.appendChild(meta);

      const del = document.createElement('button');
      del.className = 'forget';
      del.title = 'Forget this';
      del.textContent = '✕';
      del.addEventListener('click', async () => {
        await fetch('/api/memories/' + m.id, { method: 'DELETE' });
        refreshMemories();
      });
      item.appendChild(del);

      els.memoryList.appendChild(item);
    }
  } catch (e) { /* server down — status dot will show it */ }
}

/* ── jobs pane ────────────────────────────────────────────────────────────── */

// cron_jobs.next_run_at/last_run_at are unix seconds (see core/memory.h),
// unlike the SQLite UTC strings formatRelativeTime handles above.
function formatEpochRelative(epochSeconds) {
  if (!epochSeconds) return 'never';
  const mins = Math.round((epochSeconds * 1000 - Date.now()) / 60000);
  if (Math.abs(mins) < 1) return 'now';
  const ahead = mins > 0;
  const n = Math.abs(mins);
  let label;
  if (n < 60) label = n + 'm';
  else if (n < 24 * 60) label = Math.round(n / 60) + 'h';
  else label = Math.round(n / (24 * 60)) + 'd';
  return ahead ? 'in ' + label : label + ' ago';
}

async function refreshJobs() {
  try {
    const resp = await fetch('/api/jobs');
    const data = await resp.json();
    if (!data.ok) return;

    els.jobsList.innerHTML = '';
    if (data.jobs.length === 0) {
      const empty = document.createElement('div');
      empty.className = 'jobs-empty';
      empty.textContent = 'No scheduled jobs yet — ask the operator agent to schedule one.';
      els.jobsList.appendChild(empty);
      return;
    }

    for (const j of data.jobs) {
      const item = document.createElement('div');
      item.className = 'job-item';

      const title = document.createElement('div');
      title.className = 'title';
      title.append(j.name);
      const kind = document.createElement('span');
      kind.className = 'kind';
      kind.textContent = j.kind;
      title.appendChild(kind);
      item.appendChild(title);

      const target = document.createElement('div');
      target.className = 'target';
      target.textContent = j.kind === 'agent' ? '→ ' + j.agent + ': ' + j.task
                                              : '→ ' + j.command;
      item.appendChild(target);

      const meta = document.createElement('div');
      meta.className = 'meta';
      const schedule = document.createElement('span');
      schedule.textContent = j.schedule;
      meta.appendChild(schedule);
      const next = document.createElement('span');
      next.textContent = 'next ' + formatEpochRelative(j.next_run_at);
      meta.appendChild(next);
      const status = document.createElement('span');
      status.className = 'status ' + (j.running ? 'running' : (j.last_status || ''));
      status.textContent = j.running ? 'running now'
                          : j.last_status ? j.last_status + ' ' + formatEpochRelative(j.last_run_at)
                          : 'never run';
      meta.appendChild(status);
      item.appendChild(meta);

      if (j.last_output) {
        const result = document.createElement('div');
        result.className = 'result';
        const preview = document.createElement('div');
        preview.className = 'preview';
        preview.textContent = j.last_output;
        const detail = document.createElement('div');
        detail.className = 'detail';
        detail.textContent = j.last_output;
        result.append(preview, detail);
        result.addEventListener('click', () => result.classList.toggle('open'));
        item.appendChild(result);
      }

      els.jobsList.appendChild(item);
    }
  } catch (e) { /* server down — status dot will show it */ }
}

/* ── status ──────────────────────────────────────────────────────────────── */

async function refreshStatus() {
  try {
    const resp = await fetch('/api/status');
    const data = await resp.json();
    els.statusDot.className = 'dot ok';
    els.statusDot.title = data.llm.provider + ' @ ' + data.llm.url +
                          (data.semantic_memory ? ' · semantic memory' : ' · keyword memory');
    els.memoryCount.textContent = data.memories;
  } catch (e) {
    els.statusDot.className = 'dot err';
    els.statusDot.title = 'Funes server unreachable';
  }
}

function showWelcome(message) {
  els.messages.innerHTML =
    '<div class="welcome" id="welcome"><h2>“I have more memories than all mankind…”</h2>' +
    '<p>' + message + '</p></div>';
  els.welcome = $('welcome');
}

async function restoreHistory() {
  try {
    const resp = await fetch('/api/history?session=' + state.session);
    const data = await resp.json();
    if (data.ok && data.turns.length > 0) {
      for (const t of data.turns) addMessage(t.role, t.content);
    }
  } catch (e) { /* fresh chat */ }
}

/* ── conversations panel ─────────────────────────────────────────────────── */

function formatRelativeTime(sqliteUtc) {
  // memory.db timestamps are UTC "YYYY-MM-DD HH:MM:SS" — make that explicit
  // so the browser doesn't parse it as local time.
  const ms = Date.parse(sqliteUtc.replace(' ', 'T') + 'Z');
  if (Number.isNaN(ms)) return '';
  const mins = Math.max(0, Math.round((Date.now() - ms) / 60000));
  if (mins < 1)   return 'just now';
  if (mins < 60)  return mins + 'm ago';
  const hours = Math.round(mins / 60);
  if (hours < 24) return hours + 'h ago';
  return Math.round(hours / 24) + 'd ago';
}

async function refreshChats() {
  try {
    const resp = await fetch('/api/sessions');
    const data = await resp.json();
    if (!data.ok) return;

    els.chatsList.innerHTML = '';
    if (data.sessions.length === 0) {
      const empty = document.createElement('div');
      empty.className = 'chats-empty';
      empty.textContent = 'No conversations yet.';
      els.chatsList.appendChild(empty);
      return;
    }

    for (const s of data.sessions) {
      const item = document.createElement('div');
      item.className = 'chat-item' + (s.session === state.session ? ' active' : '');

      const preview = document.createElement('div');
      preview.className = 'preview';
      preview.textContent = s.preview || '(empty)';
      item.appendChild(preview);

      const meta = document.createElement('div');
      meta.className = 'meta';
      meta.textContent = formatRelativeTime(s.last_message_at) + ' · ' + s.turn_count +
                         (s.turn_count === 1 ? ' turn' : ' turns');
      item.appendChild(meta);

      item.addEventListener('click', () => switchToSession(s.session));
      els.chatsList.appendChild(item);
    }
  } catch (e) { /* server down — status dot will show it */ }
}

async function switchToSession(session) {
  if (session === state.session) return;
  state.session = session;
  localStorage.setItem('funes.session', state.session);
  state.attachments = [];
  renderAttachments();
  showWelcome('Loading this conversation…');
  await restoreHistory();
  if (els.welcome) showWelcome('Nothing here yet.');
  refreshChats();
}

/* ── wiring ──────────────────────────────────────────────────────────────── */

els.composer.addEventListener('submit', (e) => {
  e.preventDefault();
  if (state.busy) {
    if (state.abortCtrl) state.abortCtrl.abort();
    return;
  }
  const text = els.input.value.trim();
  if (!text && state.attachments.length === 0) return;

  const displayText = buildDisplayText(text, state.attachments);
  const fullText = buildFullText(text, state.attachments);
  const images = collectImages(state.attachments);
  els.input.value = '';
  els.input.style.height = 'auto';
  state.attachments = [];
  renderAttachments();
  sendMessage(displayText, fullText, images);
});

els.attachBtn.addEventListener('click', () => els.fileInput.click());

els.fileInput.addEventListener('change', async () => {
  const files = Array.from(els.fileInput.files);
  els.fileInput.value = '';
  for (const file of files) await uploadFile(file);
});

els.input.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault();
    els.composer.requestSubmit();
  }
});

els.input.addEventListener('input', () => {
  els.input.style.height = 'auto';
  els.input.style.height = Math.min(els.input.scrollHeight, 160) + 'px';
});

els.newChat.addEventListener('click', () => {
  state.session = newSessionId();
  localStorage.setItem('funes.session', state.session);
  state.attachments = [];
  renderAttachments();
  showWelcome('New conversation — but Funes still remembers everything from before.');
  els.input.focus();
  if (!els.chatsPane.hidden) refreshChats();
});

els.toggleMemory.addEventListener('click', () => {
  els.memoryPane.hidden = !els.memoryPane.hidden;
  if (!els.memoryPane.hidden) refreshMemories();
});

els.toggleChats.addEventListener('click', () => {
  els.chatsPane.hidden = !els.chatsPane.hidden;
  if (!els.chatsPane.hidden) refreshChats();
});

els.toggleJobs.addEventListener('click', () => {
  els.jobsPane.hidden = !els.jobsPane.hidden;
  if (!els.jobsPane.hidden) refreshJobs();
});

els.memorySearch.addEventListener('input', () => {
  clearTimeout(els.memorySearch._t);
  els.memorySearch._t = setTimeout(refreshMemories, 300);
});

els.memoryAdd.addEventListener('submit', async (e) => {
  e.preventDefault();
  const text = els.memoryAddText.value.trim();
  if (!text) return;
  els.memoryAddText.value = '';
  await fetch('/api/memories', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ text }),
  });
  refreshMemories();
});

/* ── boot ────────────────────────────────────────────────────────────────── */

(async function boot() {
  // Memory + conversations panes: open by default on wide screens, closed on mobile.
  if (window.innerWidth < 860) els.memoryPane.hidden = true;
  await refreshStatus();
  await restoreHistory();
  refreshMemories();
  refreshJobs();
  setInterval(refreshStatus, 15000);
  // Jobs can fire on their own timer (core/cron_runner.h), with no chat turn
  // to trigger a refresh the way schedule_job/cancel_job calls do — so this
  // pane also polls unconditionally, same as refreshStatus above.
  setInterval(refreshJobs, 15000);
  els.input.focus();
})();
