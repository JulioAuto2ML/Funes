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
  agentSelect:  $('agent-select'),
  newChat:      $('new-chat'),
  toggleMemory: $('toggle-memory'),
  memoryPane:   $('memory-pane'),
  memoryList:   $('memory-list'),
  memorySearch: $('memory-search'),
  memoryAdd:    $('memory-add'),
  memoryAddText:$('memory-add-text'),
  memoryCount:  $('memory-count'),
  semanticBadge:$('semantic-badge'),
  statusDot:    $('status-dot'),
};

const state = {
  session: localStorage.getItem('funes.session') || newSessionId(),
  agent:   localStorage.getItem('funes.agent') || '',
  busy:    false,
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

function addMessage(role, text) {
  hideWelcome();
  const div = document.createElement('div');
  div.className = 'msg ' + role;
  if (role === 'assistant') div.innerHTML = renderMarkdown(text);
  else div.textContent = text;
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

async function sendMessage(text) {
  state.busy = true;
  els.send.disabled = true;
  addMessage('user', text);

  const bubble = addMessage('assistant', '');
  bubble.classList.add('thinking');
  let streamed = '';

  try {
    const resp = await fetch('/api/chat', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        message: text,
        session: state.session,
        agent:   state.agent,
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
    bubble.innerHTML = renderMarkdown('⚠️ ' + e.message);
  } finally {
    bubble.classList.remove('thinking');
    state.busy = false;
    els.send.disabled = false;
    els.input.focus();
    refreshMemories();
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

/* ── memory pane ─────────────────────────────────────────────────────────── */

async function refreshMemories() {
  const q = els.memorySearch.value.trim();
  const params = new URLSearchParams();
  if (state.agent) params.set('agent', state.agent);
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

/* ── status + agents ─────────────────────────────────────────────────────── */

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

async function loadAgents() {
  try {
    const resp = await fetch('/api/agents');
    const data = await resp.json();
    els.agentSelect.innerHTML = '';
    for (const a of data.agents) {
      const opt = document.createElement('option');
      opt.value = a.name;
      opt.textContent = a.name;
      opt.title = a.description;
      els.agentSelect.appendChild(opt);
      if (a.is_default && !state.agent) state.agent = a.name;
    }
    if (state.agent) els.agentSelect.value = state.agent;
    state.agent = els.agentSelect.value;
  } catch (e) { /* retried on next status poll */ }
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

/* ── wiring ──────────────────────────────────────────────────────────────── */

els.composer.addEventListener('submit', (e) => {
  e.preventDefault();
  const text = els.input.value.trim();
  if (!text || state.busy) return;
  els.input.value = '';
  els.input.style.height = 'auto';
  sendMessage(text);
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

els.agentSelect.addEventListener('change', () => {
  state.agent = els.agentSelect.value;
  localStorage.setItem('funes.agent', state.agent);
  refreshMemories();
});

els.newChat.addEventListener('click', () => {
  state.session = newSessionId();
  localStorage.setItem('funes.session', state.session);
  els.messages.innerHTML =
    '<div class="welcome"><h2>“I have more memories than all mankind…”</h2>' +
    '<p>New conversation — but Funes still remembers everything from before.</p></div>';
  els.input.focus();
});

els.toggleMemory.addEventListener('click', () => {
  els.memoryPane.hidden = !els.memoryPane.hidden;
  if (!els.memoryPane.hidden) refreshMemories();
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
    body: JSON.stringify({ text, agent: state.agent }),
  });
  refreshMemories();
});

/* ── boot ────────────────────────────────────────────────────────────────── */

(async function boot() {
  // Memory pane: open by default on wide screens, closed on mobile.
  if (window.innerWidth < 860) els.memoryPane.hidden = true;
  await loadAgents();
  await refreshStatus();
  await restoreHistory();
  refreshMemories();
  setInterval(refreshStatus, 15000);
  els.input.focus();
})();
