// ── DOM refs ──────────────────────────────────────────────────────────────────
const $ = id => document.getElementById(id);
const ui = {
  form:        $('enroll-form'),
  first:       $('first-name'),
  last:        $('last-name'),
  btn:         $('enroll-btn'),
  steps:       [0, 1, 2].map(i => $(`step-${i}`)),
  lines:       [0, 1].map(i => $(`line-${i}`)),
  statusMsg:   $('status-msg'),
  log:         $('activity-log'),
  clearLog:    $('clear-log'),
  connBadge:   $('conn-badge'),
  connText:    $('conn-text'),
  usersBox:    $('users-container'),
  usersCount:  $('users-count'),
  statGranted: $('stat-granted'),
  statDenied:  $('stat-denied'),
  statLast:    $('stat-last'),
  resetDbBtn:  $('reset-db-btn'),
  // modal
  modalOverlay:    $('modal-overlay'),
  modalClose:      $('modal-close'),
  modalCancel:     $('modal-cancel'),
  editForm:        $('edit-form'),
  editId:          $('edit-id'),
  editName:        $('edit-name'),
  editRole:        $('edit-role'),
  editAccess:      $('edit-access'),
  editAccessLabel: $('edit-access-label'),
};

// ── State ─────────────────────────────────────────────────────────────────────
const state = {
  enrollPoll: null,
  eventsPoll: null,
  usersPoll:  null,
  lastEventId: 0,
  allEvents:  [],
  users:      [],
};

// ── Utilities ─────────────────────────────────────────────────────────────────
const esc = s => String(s)
  .replace(/&/g, '&amp;').replace(/</g, '&lt;')
  .replace(/>/g, '&gt;').replace(/"/g, '&quot;');

const jsonFetch = async (url, opts) => {
  try {
    const res = await fetch(url, opts);
    return { ok: res.ok, data: await res.json() };
  } catch { return { error: true }; }
};

const patchUser = async fields => {
  return jsonFetch('/api/users', {
    method: 'PATCH',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(fields),
  });
};

const deleteUser = async id => {
  return jsonFetch('/api/users', {
    method: 'DELETE',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ id }),
  });
};

// ── Connection badge ──────────────────────────────────────────────────────────
const setConn = ok => {
  ui.connBadge.className = 'conn-badge ' + (ok ? 'ok' : 'err');
  ui.connText.textContent = ok ? 'Connesso' : 'Non raggiungibile';
};

// ── Activity log ──────────────────────────────────────────────────────────────
const EVENT_META = {
  access_granted: { cls: 'ev-granted', icon: '✓' },
  access_denied:  { cls: 'ev-denied',  icon: '✗' },
  access_unknown: { cls: 'ev-denied',  icon: '✗' },
  finger_unknown: { cls: 'ev-warn',    icon: '?' },
  enroll_ok:      { cls: 'ev-enroll',  icon: '★' },
  enroll_start:   { cls: 'ev-enroll',  icon: '►' },
  enroll_error:   { cls: 'ev-error',   icon: '⚠' },
  enroll_reset:   { cls: 'ev-action',  icon: '↺' },
  db_reset:       { cls: 'ev-action',  icon: '↺' },
  error:          { cls: 'ev-error',   icon: '⚠' },
  system:         { cls: 'ev-system',  icon: 'ℹ' },
};

const addLogEntry = (type, message, ts) => {
  const meta = EVENT_META[type] || { cls: 'ev-system', icon: '·' };
  const time = ts ? ts.split(' ')[1] || ts : new Date().toLocaleTimeString('it-IT');
  const div = document.createElement('div');
  div.className = `log-entry ${meta.cls}`;
  div.innerHTML =
    `<span class="log-ts">${esc(time)}</span>` +
    `<span class="log-icon">${meta.icon}</span>` +
    `<span class="log-type">${esc(type)}</span>` +
    `<span class="log-msg">${esc(message)}</span>`;
  ui.log.prepend(div);
  while (ui.log.children.length > 150) ui.log.removeChild(ui.log.lastChild);
};

// ── Stats ─────────────────────────────────────────────────────────────────────
const updateStats = () => {
  const granted = state.allEvents.filter(e => e.type === 'access_granted').length;
  const denied  = state.allEvents.filter(
    e => e.type === 'access_denied' || e.type === 'access_unknown'
  ).length;
  const lastGrant = [...state.allEvents].reverse().find(e => e.type === 'access_granted');

  ui.statGranted.textContent = granted;
  ui.statDenied.textContent  = denied;
  if (lastGrant) {
    const parts = lastGrant.ts.split(' ');
    ui.statLast.textContent = parts.length > 1 ? parts[1] : lastGrant.ts;
  } else {
    ui.statLast.textContent = '—';
  }
};

// ── Enrollment steps ──────────────────────────────────────────────────────────
const STATUS_MSG = {
  idle:        'In attesa di una nuova registrazione.',
  in_progress: 'Avvicina il dito al sensore…',
  done:        'Impronta registrata con successo.',
  error:       'Errore durante la registrazione.',
};

const setStep = statusType => {
  ui.steps.forEach(el => el.classList.remove('active', 'done', 'error'));
  ui.lines.forEach(l  => l.classList.remove('done'));

  if (statusType === 'idle') {
    ui.steps[0].classList.add('active');
  } else if (statusType === 'in_progress') {
    ui.steps[0].classList.add('done');
    ui.lines[0].classList.add('done');
    ui.steps[1].classList.add('active');
  } else if (statusType === 'done') {
    ui.steps.forEach(el => el.classList.add('done'));
    ui.lines.forEach(l  => l.classList.add('done'));
  } else if (statusType === 'error') {
    ui.steps[0].classList.add('done');
    ui.lines[0].classList.add('done');
    ui.steps[1].classList.add('done');
    ui.steps[2].classList.add('error');
  }
  ui.statusMsg.textContent = STATUS_MSG[statusType] || statusType;
};

// ── Modal ─────────────────────────────────────────────────────────────────────
const openModal = user => {
  ui.editId.value          = user.id;
  ui.editName.value        = user.name;
  ui.editRole.value        = user.role;
  ui.editAccess.checked    = user.has_access;
  ui.editAccessLabel.textContent = user.has_access ? 'Attivo' : 'Revocato';
  ui.modalOverlay.hidden   = false;
};

const closeModal = () => { ui.modalOverlay.hidden = true; };

ui.editAccess?.addEventListener('change', () => {
  ui.editAccessLabel.textContent = ui.editAccess.checked ? 'Attivo' : 'Revocato';
});

ui.modalClose?.addEventListener('click',  closeModal);
ui.modalCancel?.addEventListener('click', closeModal);
ui.modalOverlay?.addEventListener('click', e => {
  if (e.target === ui.modalOverlay) closeModal();
});

ui.editForm?.addEventListener('submit', async e => {
  e.preventDefault();
  const id = parseInt(ui.editId.value, 10);
  const res = await patchUser({
    id,
    name:       ui.editName.value.trim(),
    role:       ui.editRole.value.trim(),
    has_access: ui.editAccess.checked,
  });
  if (res.error || !res.data?.ok) {
    alert('Errore durante il salvataggio.');
    return;
  }
  closeModal();
  fetchUsers();
});

// ── Users table ───────────────────────────────────────────────────────────────
const renderUsers = users => {
  state.users = users || [];
  if (state.users.length === 0) {
    ui.usersBox.innerHTML = '<div class="placeholder">Nessun utente registrato.</div>';
    ui.usersCount.textContent = '';
    return;
  }
  ui.usersCount.textContent = `${state.users.length} utent${state.users.length === 1 ? 'e' : 'i'}`;

  let html =
    '<table class="users-table">' +
    '<thead><tr><th>ID</th><th>Nome</th><th>Ruolo</th><th>Accesso</th><th></th><th></th></tr></thead>' +
    '<tbody>';
  for (const u of state.users) {
    const badge = u.has_access
      ? `<button class="access-badge yes" data-id="${u.id}" data-access="true">● Attivo</button>`
      : `<button class="access-badge no"  data-id="${u.id}" data-access="false">○ Revocato</button>`;
    html +=
      `<tr>` +
      `<td class="user-id">${esc(u.id)}</td>` +
      `<td class="user-name">${esc(u.name)}</td>` +
      `<td class="user-role">${esc(u.role)}</td>` +
      `<td>${badge}</td>` +
      `<td><button class="edit-btn" data-id="${u.id}" title="Modifica">✎</button></td>` +
      `<td><button class="del-btn" data-id="${u.id}" data-name="${esc(u.name)}" title="Elimina">🗑</button></td>` +
      `</tr>`;
  }
  html += '</tbody></table>';
  ui.usersBox.innerHTML = html;

  // Toggle accesso
  ui.usersBox.querySelectorAll('.access-badge').forEach(btn => {
    btn.addEventListener('click', async () => {
      const id        = parseInt(btn.dataset.id, 10);
      const newAccess = btn.dataset.access !== 'true';
      btn.disabled = true;
      const res = await patchUser({ id, has_access: newAccess });
      if (res.error || !res.data?.ok) { btn.disabled = false; return; }
      fetchUsers();
    });
  });

  // Apri modal modifica
  ui.usersBox.querySelectorAll('.edit-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const id   = parseInt(btn.dataset.id, 10);
      const user = state.users.find(u => u.id === id);
      if (user) openModal(user);
    });
  });

  // Elimina utente
  ui.usersBox.querySelectorAll('.del-btn').forEach(btn => {
    btn.addEventListener('click', async () => {
      const id   = parseInt(btn.dataset.id, 10);
      const name = btn.dataset.name;
      if (!confirm(`Eliminare "${name}" (slot ${id})?\nL'impronta verrà rimossa dall'AS608 e dal database.`)) return;
      btn.disabled = true;
      const res = await deleteUser(id);
      btn.disabled = false;
      if (res.error) { alert('Errore di connessione.'); return; }
      if (!res.data?.ok) {
        alert('Eliminazione fallita: il sensore AS608 non ha liberato lo slot.\nL\'utente non è stato rimosso.');
        return;
      }
      fetchUsers();
    });
  });
};

const fetchUsers = async () => {
  const res = await jsonFetch('/api/users');
  if (res.error || !res.data?.ok) {
    if (!ui.usersBox.querySelector('table')) {
      ui.usersBox.innerHTML = '<div class="placeholder">Impossibile caricare gli utenti.</div>';
    }
    return;
  }
  renderUsers(res.data.users || []);
};

// ── Events polling ────────────────────────────────────────────────────────────
const fetchEvents = async () => {
  const res = await jsonFetch(`/api/events?since=${state.lastEventId}`);
  if (res.error) { setConn(false); return; }
  if (!res.data?.ok) return;
  setConn(true);

  for (const ev of res.data.events || []) {
    addLogEntry(ev.type, ev.message, ev.ts);
    state.allEvents.push(ev);
    if (ev.id > state.lastEventId) state.lastEventId = ev.id;
  }
  updateStats();
};

// ── Status polling (during enrollment) ───────────────────────────────────────
const stopEnrollPoll = () => {
  clearInterval(state.enrollPoll);
  state.enrollPoll = null;
};

const fetchStatus = async () => {
  const res = await jsonFetch('/api/status');
  if (res.error || !res.data?.ok) return;
  const s = res.data.status || 'idle';
  setStep(s);

  if (s === 'done') {
    stopEnrollPoll();
    ui.btn.disabled = false;
    fetchUsers();
  } else if (s === 'error') {
    stopEnrollPoll();
    ui.btn.disabled = false;
  } else if (s === 'in_progress' && !state.enrollPoll) {
    ui.btn.disabled = true;
    state.enrollPoll = setInterval(fetchStatus, 1500);
  }
};

// ── Enroll form ───────────────────────────────────────────────────────────────
ui.form?.addEventListener('submit', async e => {
  e.preventDefault();
  const first = ui.first.value.trim();
  const last  = ui.last.value.trim();
  if (!first || !last) return;

  ui.btn.disabled = true;
  setStep('in_progress');

  const res = await jsonFetch('/api/enroll', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ first_name: first, last_name: last }),
  });

  if (res.error || !res.ok || !res.data?.ok) {
    setStep('error');
    ui.statusMsg.textContent = `Errore: ${res.data?.error || 'richiesta fallita'}`;
    ui.btn.disabled = false;
    return;
  }

  state.enrollPoll = setInterval(fetchStatus, 1500);
});

// ── Clear log ─────────────────────────────────────────────────────────────────
ui.clearLog?.addEventListener('click', () => { ui.log.innerHTML = ''; });

// ── Reset database ────────────────────────────────────────────────────────────
ui.resetDbBtn?.addEventListener('click', async () => {
  const confirmed = confirm(
    'Attenzione: questa operazione cancellerà TUTTE le impronte dal sensore AS608 ' +
    'e tutti gli utenti dal database.\n\nProcedere?'
  );
  if (!confirmed) return;

  ui.resetDbBtn.disabled = true;
  ui.resetDbBtn.textContent = 'Azzeramento…';

  const res = await jsonFetch('/api/reset', { method: 'POST' });

  ui.resetDbBtn.disabled = false;
  ui.resetDbBtn.textContent = 'Azzera DB';

  if (res.error) {
    alert('Errore: impossibile raggiungere l\'ESP32.');
    return;
  }

  if (!res.data?.ok) {
    alert('Reset annullato: il sensore AS608 non risponde.\nNessun dato è stato eliminato.');
    return;
  }

  alert('Reset completato: impronte AS608 e database SQLite azzerati.');
  fetchUsers();
});

// ── Init ──────────────────────────────────────────────────────────────────────
setStep('idle');
fetchEvents();
fetchUsers();
fetchStatus();
state.eventsPoll = setInterval(fetchEvents, 2000);
state.usersPoll  = setInterval(fetchUsers,  12000);
