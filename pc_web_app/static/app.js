const $ = (id) => document.getElementById(id);
const ui = {
  form: $("enroll-form"),
  first: $("first-name"),
  last: $("last-name"),
  badge: $("status-badge"),
  details: $("status-details"),
  id: $("status-id"),
  log: $("activity-log")
};

const MAX_LOG_LINES = 120;
const STATUS_POLL_MS = 1500;
const EVENTS_POLL_MS = 2000;
const state = { lines: [], poll: null, events: null, lastEventId: 0 };

const addLog = (line, ts) => {
  const time = ts || new Date().toLocaleTimeString();
  state.lines.unshift(`[${time}] ${line}`);
  if (state.lines.length > MAX_LOG_LINES) state.lines.length = MAX_LOG_LINES;
  ui.log.textContent = state.lines.join("\n");
};

const setStatus = (status, message, id) => {
  ui.badge.textContent = status.replace(/_/g, " ");
  ui.details.textContent = message || "";
  ui.id.textContent = id ? `ID: ${id}` : "";
};

const jsonFetch = async (url, options) => {
  try {
    const res = await fetch(url, options);
    return { ok: res.ok, data: await res.json() };
  } catch (err) {
    return { error: true };
  }
};

const fetchStatus = async () => {
  const result = await jsonFetch("/api/status");
  if (result.error) {
    setStatus("error", "Failed to reach server", "");
    addLog("Network error while reading status");
    stopPolling();
    return;
  }

  const data = result.data || {};
  if (!data.ok) {
    setStatus("error", data.error || "unknown error", "");
    addLog(`Status error: ${data.error || "unknown"}`);
    stopPolling();
    return;
  }

  const current = data.status || "idle";
  setStatus(current, data.message || "", data.id || "");

  if (current === "done") {
    addLog(`Enrollment complete for ${data.first_name} ${data.last_name} (ID ${data.id})`);
    stopPolling();
  } else if (current === "error") {
    addLog(`Enrollment failed: ${data.message || "unknown"}`);
    stopPolling();
  }
};

const fetchEvents = async () => {
  const result = await jsonFetch(`/api/events?since=${state.lastEventId}`);
  if (result.error) return addLog("Network error while reading events");

  const data = result.data || {};
  if (!data.ok) return addLog(`Events error: ${data.error || "unknown"}`);

  for (const event of data.events || []) {
    addLog(`${event.type}: ${event.message}`, event.ts);
    if (event.id > state.lastEventId) state.lastEventId = event.id;
  }
};

const startPolling = () => {
  if (!state.poll) state.poll = setInterval(fetchStatus, STATUS_POLL_MS);
};

const stopPolling = () => {
  if (!state.poll) return;
  clearInterval(state.poll);
  state.poll = null;
};

const startEventPolling = () => {
  if (!state.events) state.events = setInterval(fetchEvents, EVENTS_POLL_MS);
};

ui.form?.addEventListener("submit", async (event) => {
  event.preventDefault();

  const first = ui.first.value.trim();
  const last = ui.last.value.trim();
  if (!first || !last) return addLog("Insert both first and last name");

  setStatus("in_progress", "Starting enrollment", "");
  addLog(`Starting enrollment for ${first} ${last}`);

  const result = await jsonFetch("/api/enroll", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ first_name: first, last_name: last })
  });

  if (result.error) {
    setStatus("error", "Failed to reach server", "");
    return addLog("Network error while starting enrollment");
  }

  const data = result.data || {};
  if (!result.ok || !data.ok) {
    setStatus("error", data.error || "request failed", "");
    return addLog(`Enroll request failed: ${data.error || "request failed"}`);
  }

  setStatus("in_progress", "Waiting for fingerprint sensor", "");
  startPolling();
});

fetchStatus();
fetchEvents();
startEventPolling();
