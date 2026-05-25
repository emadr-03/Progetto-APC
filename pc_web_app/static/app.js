const form = document.getElementById("enroll-form");
const firstNameInput = document.getElementById("first-name");
const lastNameInput = document.getElementById("last-name");
const statusBadge = document.getElementById("status-badge");
const statusDetails = document.getElementById("status-details");
const statusId = document.getElementById("status-id");
const logEl = document.getElementById("activity-log");

let pollTimer = null;
let eventTimer = null;
let lastEventId = 0;

function addLog(line, ts) {
  const time = ts || new Date().toLocaleTimeString();
  logEl.textContent = `[${time}] ${line}\n` + logEl.textContent;
}

function setStatus(state, message, id) {
  statusBadge.textContent = state.replace("_", " ");
  statusDetails.textContent = message || "";
  statusId.textContent = id ? `ID: ${id}` : "";
}

async function fetchStatus() {
  try {
    const res = await fetch("/api/status");
    const data = await res.json();
    if (!data.ok) {
      setStatus("error", data.error || "unknown error", "");
      addLog(`Status error: ${data.error || "unknown"}`);
      stopPolling();
      return;
    }

    const state = data.status || "idle";
    setStatus(state, data.message || "", data.id || "");

    if (state === "done") {
      addLog(`Enrollment complete for ${data.first_name} ${data.last_name} (ID ${data.id})`);
      stopPolling();
    } else if (state === "error") {
      addLog(`Enrollment failed: ${data.message || "unknown"}`);
      stopPolling();
    }
  } catch (err) {
    setStatus("error", "Failed to reach server", "");
    addLog("Network error while reading status");
    stopPolling();
  }
}

async function fetchEvents() {
  try {
    const res = await fetch(`/api/events?since=${lastEventId}`);
    const data = await res.json();
    if (!data.ok) {
      addLog(`Events error: ${data.error || "unknown"}`);
      return;
    }

    const events = data.events || [];
    for (const event of events) {
      const label = `${event.type}: ${event.message}`;
      addLog(label, event.ts);
      if (event.id > lastEventId) lastEventId = event.id;
    }
  } catch (err) {
    addLog("Network error while reading events");
  }
}

function startPolling() {
  if (pollTimer) return;
  pollTimer = setInterval(fetchStatus, 1500);
}

function stopPolling() {
  if (!pollTimer) return;
  clearInterval(pollTimer);
  pollTimer = null;
}

function startEventPolling() {
  if (eventTimer) return;
  eventTimer = setInterval(fetchEvents, 2000);
}

if (form) {
  form.addEventListener("submit", async (event) => {
    event.preventDefault();

    const firstName = firstNameInput.value.trim();
    const lastName = lastNameInput.value.trim();

    if (!firstName || !lastName) {
      addLog("Insert both first and last name");
      return;
    }

    setStatus("in_progress", "Starting enrollment", "");
    addLog(`Starting enrollment for ${firstName} ${lastName}`);

    try {
      const res = await fetch("/api/enroll", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ first_name: firstName, last_name: lastName })
      });

      const data = await res.json();
      if (!res.ok || !data.ok) {
        setStatus("error", data.error || "request failed", "");
        addLog(`Enroll request failed: ${data.error || "request failed"}`);
        return;
      }

      setStatus("in_progress", "Waiting for fingerprint sensor", "");
      startPolling();
    } catch (err) {
      setStatus("error", "Failed to reach server", "");
      addLog("Network error while starting enrollment");
    }
  });
}

fetchStatus();
fetchEvents();
startEventPolling();
