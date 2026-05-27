/* esp32_smartlock.ino — MERGED (DEBUG + v3)
   UART2: GPIO16=RX (da PA9-USART1_TX STM32), GPIO17=TX (verso PA10-USART1_RX STM32)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <time.h>
#include <LittleFS.h>
#include <sqlite3.h>

/* ── Credenziali ─────────────────────────────────── */
const char* SSID      = "iPhone_MrDome";
const char* WIFI_PASS = "nonlasos";
const char* API_KEY   = "Pendragon";     /* X-API-KEY per le chiamate HTTP */

/* ── UART verso STM32 ────────────────────────────── */
#define STM_RX   16    /* <- PA9  (USART1_TX STM32) */
#define STM_TX   17    /* -> PA10 (USART1_RX STM32) */
#define STM_BAUD 115200

/* ── Database utenti ─────────────────────────────
   id       = ID salvato nell'AS608 (0-127)
   name     = nome visualizzato su Telegram
   role     = ruolo descrittivo
   hasAccess= true -> GRANTED, false -> DENIED        */
struct User {
    uint8_t id;
    char    name[64];
    char    role[32];
    bool    hasAccess;
    bool    found;      // false se la query non ha trovato righe
};

sqlite3* db = nullptr;
const char* DB_PATH = "/littlefs/smartlock.db";

enum EnrollState {
    ENROLL_IDLE = 0,
    ENROLL_IN_PROGRESS,
    ENROLL_DONE,
    ENROLL_ERROR
};

struct EnrollContext {
    EnrollState state;
    char first[32];
    char last[32];
    uint8_t id;
    char message[64];
    uint32_t startedMs;
    uint32_t completedMs;
};

#define ENROLL_RESET_MS 10000

EnrollContext enroll = { ENROLL_IDLE, "", "", 0, "idle", 0, 0 };

struct EventLog {
    uint32_t id;
    char ts[20];
    char type[20];
    char message[120];
};

#define MAX_EVENTS 30
EventLog events[MAX_EVENTS];
uint8_t  eventSize = 0;
uint8_t  eventHead = 0;
uint32_t nextEventId = 1;

/* ── Prototipi ───────────────────────────────────── */
void    handle(const char* cmd);
String  timestamp();
void    setupServer();
void    handleApiEnroll();
void    handleApiEnrollStatus();
void    handleApiHealth();
void    handleApiEvents();
void    handleApiUsers();
void    handleApiUserPatch();
void    handleApiUserDelete();
void    handleApiReset();
void    startEnrollment(const char* first, const char* last);
void    onEnrollResult(const char* payload);
void    addEvent(const char* type, const char* message);
void    resetEnrollment();
bool    resetUserDatabase();
void    setEnrollError(const char* reason);
void    setEnrollDone(uint8_t id, const char* fullName);
bool    checkApiKey();
void    sendJson(int code, const char* body);

/* ── Oggetti ─────────────────────────────────────── */
HardwareSerial stm(2);      /* UART2 ESP32 */
WebServer      server(80);

/* ── Buffer UART ─────────────────────────────────── */
char    rxBuf[128];
uint8_t rxIdx    = 0;
bool    rxDiscard = false;   /* true = riga corrente contaminata, scarta fino a '\n' */

/* ══════════════════════════════════════════════════ */

/* Apre (o crea) il DB e popola la tabella utenti se vuota */
void initDB() {
    if (!LittleFS.begin(true)) {   // true = formatta se corrotto
        return;
    }

    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        return;
    }

    const char* createSQL =
        "CREATE TABLE IF NOT EXISTS users ("
        "  id         INTEGER PRIMARY KEY,"   /* ID AS608 (0-127) */
        "  name       TEXT    NOT NULL,"
        "  role       TEXT    NOT NULL,"
        "  has_access INTEGER NOT NULL DEFAULT 1"  /* 1=GRANTED, 0=DENIED */
        ");";

    char* errMsg = nullptr;

    if (sqlite3_exec(db, createSQL, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        sqlite3_free(errMsg);
        return;
    }

}

User findUser(uint8_t targetId) {
    User u = {};

    if (!db) {
        return u;
    }

    const char* sql =
        "SELECT id, name, role, has_access FROM users WHERE id = ? LIMIT 1;";

    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return u;
    }
    sqlite3_bind_int(stmt, 1, targetId);   // binding sicuro, no SQL injection

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        u.id        = (uint8_t)sqlite3_column_int(stmt, 0);
        strncpy(u.name, (const char*)sqlite3_column_text(stmt, 1), sizeof(u.name) - 1);
        strncpy(u.role, (const char*)sqlite3_column_text(stmt, 2), sizeof(u.role) - 1);
        u.hasAccess = sqlite3_column_int(stmt, 3) == 1;
        u.found     = true;
    }

    sqlite3_finalize(stmt);
    return u;
}

/* Aggiunge o aggiorna un utente */
bool update_or_insertUser(uint8_t id, const char* name, const char* role, bool access) {
    if (!db) {
        return false;
    }

    const char* updateSql =
        "UPDATE users SET name = ?, role = ?, has_access = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, updateSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, role, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 3, access ? 1 : 0);
    sqlite3_bind_int (stmt, 4, id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return false;
    }

    int changes = sqlite3_changes(db);
    sqlite3_finalize(stmt);

    if (changes > 0) {
        return true;
    }

    const char* insertSql =
        "INSERT INTO users (id, name, role, has_access) VALUES (?,?,?,?);";

    if (sqlite3_prepare_v2(db, insertSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int (stmt, 1, id);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, role, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 4, access ? 1 : 0);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

/* Elimina un utente dal DB */
bool deleteUser(uint8_t id) {
    const char* sql = "DELETE FROM users WHERE id = ?;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int(stmt, 1, id);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

void addEvent(const char* type, const char* message) {
    EventLog* e = &events[eventHead];
    e->id = nextEventId++;

    String ts = timestamp();
    strncpy(e->ts, ts.c_str(), sizeof(e->ts) - 1);
    e->ts[sizeof(e->ts) - 1] = '\0';

    strncpy(e->type, type, sizeof(e->type) - 1);
    e->type[sizeof(e->type) - 1] = '\0';

    strncpy(e->message, message, sizeof(e->message) - 1);
    e->message[sizeof(e->message) - 1] = '\0';

    eventHead = (eventHead + 1) % MAX_EVENTS;
    if (eventSize < MAX_EVENTS) eventSize++;
}

void resetEnrollment() {
    enroll.state = ENROLL_IDLE;
    enroll.first[0] = '\0';
    enroll.last[0]  = '\0';
    enroll.id = 0;
    snprintf(enroll.message, sizeof(enroll.message), "idle");
    enroll.startedMs   = 0;
    enroll.completedMs = 0;
}

bool resetUserDatabase() {
    if (!db) {
        addEvent("error", "DB reset fallito: database non aperto");
        return false;
    }
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, "DELETE FROM users;", nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        char msg[80];
        snprintf(msg, sizeof(msg), "DB reset fallito: %s",
                 errMsg ? errMsg : "errore sconosciuto");
        sqlite3_free(errMsg);
        addEvent("error", msg);
        return false;
    }
    resetEnrollment();
    addEvent("db_reset", "User database reset");
    return true;
}

void sendJson(int code, const char* body) {
    server.send(code, "application/json", body);
}

bool checkApiKey() {
    if (!server.hasHeader("X-API-KEY")) {
        sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
        return false;
    }

    if (server.header("X-API-KEY") != API_KEY) {
        sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
        return false;
    }

    return true;
}

void handleApiHealth() {
    if (!checkApiKey()) return;
    sendJson(200, "{\"ok\":true}");
}

void handleApiEvents() {
    if (!checkApiKey()) return;

    uint32_t since = 0;
    if (server.hasArg("since")) {
        since = (uint32_t)atoi(server.arg("since").c_str());
    }

    DynamicJsonDocument doc(4096);
    doc["ok"] = true;
    JsonArray arr = doc.createNestedArray("events");

    uint8_t start = (eventHead + MAX_EVENTS - eventSize) % MAX_EVENTS;
    for (uint8_t i = 0; i < eventSize; i++) {
        uint8_t idx = (start + i) % MAX_EVENTS;
        EventLog* e = &events[idx];
        if (e->id <= since) continue;

        JsonObject item = arr.createNestedObject();
        item["id"]      = e->id;
        item["ts"]      = e->ts;
        item["type"]    = e->type;
        item["message"] = e->message;
    }

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

void startEnrollment(const char* first, const char* last) {
    strncpy(enroll.first, first, sizeof(enroll.first) - 1);
    strncpy(enroll.last,  last,  sizeof(enroll.last)  - 1);
    enroll.first[sizeof(enroll.first) - 1] = '\0';
    enroll.last [sizeof(enroll.last)  - 1] = '\0';

    enroll.id          = 0;
    enroll.state       = ENROLL_IN_PROGRESS;
    enroll.completedMs = 0;

    snprintf(enroll.message, sizeof(enroll.message), "waiting_for_sensor");

    enroll.startedMs = millis();
    stm.println("ENROLL_START");

    char msg[128];
    snprintf(msg, sizeof(msg), "Enrollment started for %s %s",
             enroll.first, enroll.last);
    addEvent("enroll_start", msg);
}

void handleApiEnroll() {
    if (!checkApiKey()) return;

    if (enroll.state == ENROLL_IN_PROGRESS) {
        sendJson(409, "{\"ok\":false,\"error\":\"enroll_in_progress\"}");
        return;
    }

    String body = server.arg("plain");
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        sendJson(400, "{\"ok\":false,\"error\":\"bad_json\"}");
        return;
    }

    const char* first = doc["first_name"] | "";
    const char* last  = doc["last_name"]  | "";
    if (strlen(first) == 0 || strlen(last) == 0) {
        sendJson(400, "{\"ok\":false,\"error\":\"missing_name\"}");
        return;
    }

    startEnrollment(first, last);
    sendJson(202, "{\"ok\":true,\"status\":\"started\"}");
}

void handleApiEnrollStatus() {
    if (!checkApiKey()) return;

    StaticJsonDocument<256> doc;
    doc["ok"] = true;

    switch (enroll.state) {
        case ENROLL_IDLE:        doc["status"] = "idle";        break;
        case ENROLL_IN_PROGRESS: doc["status"] = "in_progress"; break;
        case ENROLL_DONE:        doc["status"] = "done";        break;
        case ENROLL_ERROR:       doc["status"] = "error";       break;
        default:                 doc["status"] = "unknown";     break;
    }

    doc["first_name"] = enroll.first;
    doc["last_name"]  = enroll.last;
    doc["id"]         = enroll.id;
    doc["message"]    = enroll.message;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

void setupServer() {
    const char* headers[] = { "X-API-KEY" };
    server.collectHeaders(headers, 1);

    server.on("/api/health",        HTTP_GET,  handleApiHealth);
    server.on("/api/enroll",        HTTP_POST, handleApiEnroll);
    server.on("/api/enroll/status", HTTP_GET,  handleApiEnrollStatus);
    server.on("/api/events",        HTTP_GET,  handleApiEvents);
    server.on("/api/users",         HTTP_GET,    handleApiUsers);
    server.on("/api/users",         HTTP_PATCH,  handleApiUserPatch);
    server.on("/api/users",         HTTP_DELETE, handleApiUserDelete);
    server.on("/api/reset",         HTTP_POST,   handleApiReset);
    server.begin();
}

void onEnrollResult(const char* payload) {
    if (strncmp(payload, "ERR:", 4) == 0) {
        setEnrollError(payload + 4);
        return;
    }

    const char* colon = strchr(payload, ':');
    if (!colon) {
        setEnrollError("bad_result");
        return;
    }

    uint8_t     id     = (uint8_t)atoi(payload);
    const char* status = colon + 1;

    if (strcmp(status, "OK") != 0) {
        char reason[64];
        snprintf(reason, sizeof(reason), "status_%s", status);
        setEnrollError(reason);
        return;
    }

    char fullName[64];
    snprintf(fullName, sizeof(fullName), "%s %s", enroll.first, enroll.last);

    if (!update_or_insertUser(id, fullName, "User", true)) {
        const char* err = db ? sqlite3_errmsg(db) : "db_null";
        char reason[96];
        snprintf(reason, sizeof(reason), "db_write_failed:%s", err);
        setEnrollError(reason);
        return;
    }

    setEnrollDone(id, fullName);
}

void setup() {
    Serial.begin(115200);
    stm.begin(STM_BAUD, SERIAL_8N1, STM_RX, STM_TX);
    WiFi.begin(SSID, WIFI_PASS);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
    }

    /* NTP Italia - CEST (ora legale) UTC+2 */
    configTime(3600, 3600, "pool.ntp.org");

    initDB();
    setupServer();
    int userCount = 0;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM users;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            userCount = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    {
        char msg[128];
        snprintf(msg, sizeof(msg), "Smart Lock Online - IP %s - Users %d",
                 WiFi.localIP().toString().c_str(), userCount);
        addEvent("system", msg);
    }

}

void loop() {
    while (stm.available()) {
        char c = (char)stm.read();

        if (c == '\n') {
            /* Fine riga: processa solo se non contaminata da byte spuri */
            if (!rxDiscard) {
                rxBuf[rxIdx] = '\0';
                if (rxIdx > 0) handle(rxBuf);
            }
            rxIdx    = 0;
            rxDiscard = false;
        } else if (c != '\r') {
            if ((uint8_t)c < 32 || (uint8_t)c > 126) {
                /* Byte non-ASCII (EMI): scarta l'intera riga corrente.
                 * Azzerare solo rxIdx non basta — il resto della riga
                 * (es. "TH_REQUEST:1") verrebbe processato parziale.  */
                rxDiscard = true;
                rxIdx     = 0;
            } else if (!rxDiscard && rxIdx < 126) {
                rxBuf[rxIdx++] = c;
            }
        }
    }

    server.handleClient();

    /* Timeout enrollment */
    if (enroll.state == ENROLL_IN_PROGRESS &&
        (millis() - enroll.startedMs > 120000)) {
        setEnrollError("timeout");
    }

    /* Auto-reset dopo completamento/errore */
    if ((enroll.state == ENROLL_ERROR || enroll.state == ENROLL_DONE) &&
        enroll.completedMs > 0 &&
        (millis() - enroll.completedMs > ENROLL_RESET_MS)) {
        resetEnrollment();
        addEvent("enroll_reset", "Enrollment state reset");
    }

    delay(10);
}

/* ── Parser comandi ──────────────────────────────── */
void handle(const char* cmd) {
    /* ── ENROLL_RESULT:<id>:OK | ENROLL_RESULT:ERR:<reason> ── */
    if (strncmp(cmd, "ENROLL_RESULT:", 14) == 0) {
        onEnrollResult(cmd + 14);
        return;
    }

    if (strcmp(cmd, "DB_RESET") == 0) {
        resetUserDatabase();
        return;
    }

    /* ── AUTH_REQUEST:<id> ── */
    if (strncmp(cmd, "AUTH_REQUEST:", 13) == 0) {
        uint8_t reqId = (uint8_t)atoi(cmd + 13);
        User u = findUser(reqId);
        String resp = "AUTH_RESULT:" + String(reqId) + ":";

        if (u.found && u.hasAccess) {
            stm.println(resp + "GRANTED");
            char msg[128];
            snprintf(msg, sizeof(msg), "Access granted: %s (ID %u)",
                     u.name, (unsigned)reqId);
            addEvent("access_granted", msg);

        } else if (u.found && !u.hasAccess) {
            stm.println(resp + "DENIED");
            char msg[128];
            snprintf(msg, sizeof(msg), "Access denied: %s (ID %u)",
                     u.name, (unsigned)reqId);
            addEvent("access_denied", msg);

        } else {
            stm.println(resp + "DENIED");
            char msg[128];
            snprintf(msg, sizeof(msg), "Unknown ID: %u", (unsigned)reqId);
            addEvent("access_unknown", msg);
        }
        return;
    }

    /* ── Impronta sconosciuta (AS608 no-match) ── */
    if (strcmp(cmd, "ALERT:UNKNOWN_FINGER") == 0) {
        addEvent("finger_unknown", "Unrecognized fingerprint");
        return;
    }

    /* ── Errori generici ── */
    if (strncmp(cmd, "ERR:", 4) == 0) {
        addEvent("error", cmd + 4);
        return;
    }
}

void handleApiUserPatch() {
    if (!checkApiKey()) return;

    String body = server.arg("plain");
    StaticJsonDocument<256> req;
    if (deserializeJson(req, body)) {
        sendJson(400, "{\"ok\":false,\"error\":\"bad_json\"}");
        return;
    }

    if (!req.containsKey("id")) {
        sendJson(400, "{\"ok\":false,\"error\":\"missing_id\"}");
        return;
    }

    uint8_t id = (uint8_t)req["id"].as<int>();
    User u = findUser(id);
    if (!u.found) {
        sendJson(404, "{\"ok\":false,\"error\":\"not_found\"}");
        return;
    }

    const char* name      = req.containsKey("name")       ? req["name"].as<const char*>() : u.name;
    const char* role      = req.containsKey("role")       ? req["role"].as<const char*>() : u.role;
    bool        hasAccess = req.containsKey("has_access") ? req["has_access"].as<bool>()  : u.hasAccess;

    if (!update_or_insertUser(id, name, role, hasAccess)) {
        sendJson(500, "{\"ok\":false,\"error\":\"db_error\"}");
        return;
    }

    sendJson(200, "{\"ok\":true}");
}

void handleApiUsers() {
    if (!checkApiKey()) return;

    DynamicJsonDocument doc(4096);
    doc["ok"] = true;
    JsonArray arr = doc.createNestedArray("users");

    if (db) {
        const char* sql = "SELECT id, name, role, has_access FROM users ORDER BY id;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                JsonObject u = arr.createNestedObject();
                u["id"]         = sqlite3_column_int(stmt, 0);
                u["name"]       = (const char*)sqlite3_column_text(stmt, 1);
                u["role"]       = (const char*)sqlite3_column_text(stmt, 2);
                u["has_access"] = sqlite3_column_int(stmt, 3) == 1;
            }
            sqlite3_finalize(stmt);
        }
    }

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

void setEnrollError(const char* reason) {
    enroll.state = ENROLL_ERROR;
    snprintf(enroll.message, sizeof(enroll.message), "%s", reason);
    enroll.completedMs = millis();

    char msg[128];
    snprintf(msg, sizeof(msg), "Enrollment failed: %s", enroll.message);
    addEvent("enroll_error", msg);
}

void setEnrollDone(uint8_t id, const char* fullName) {
    enroll.id          = id;
    enroll.state       = ENROLL_DONE;
    enroll.completedMs = millis();
    snprintf(enroll.message, sizeof(enroll.message), "saved");

    char msg[128];
    snprintf(msg, sizeof(msg), "Enrollment ok: %s (ID %u)", fullName, (unsigned)id);
    addEvent("enroll_ok", msg);
}

void handleApiUserDelete() {
    if (!checkApiKey()) return;

    String body = server.arg("plain");
    StaticJsonDocument<128> req;
    if (deserializeJson(req, body) || !req.containsKey("id")) {
        sendJson(400, "{\"ok\":false,\"error\":\"missing_id\"}");
        return;
    }

    uint8_t id = (uint8_t)req["id"].as<int>();
    User u = findUser(id);
    if (!u.found) {
        sendJson(404, "{\"ok\":false,\"error\":\"not_found\"}");
        return;
    }

    /* Invia DELETE_CHAR:<id> alla STM32 e attendi conferma (max 5 s) */
    while (stm.available()) stm.read();
    {
        char cmd[24];
        snprintf(cmd, sizeof(cmd), "DELETE_CHAR:%u", (unsigned)id);
        stm.println(cmd);
    }

    char     respBuf[32];
    uint8_t  respIdx  = 0;
    bool     stm_ok   = false;
    bool     got_resp = false;
    uint32_t t0       = millis();

    while (!got_resp && millis() - t0 < 5000) {
        while (stm.available()) {
            char c = (char)stm.read();
            if (c == '\n') {
                respBuf[respIdx] = '\0';
                char expected_ok[24], expected_err[24];
                snprintf(expected_ok,  sizeof(expected_ok),  "DELETE_CHAR_OK:%u",  (unsigned)id);
                snprintf(expected_err, sizeof(expected_err), "DELETE_CHAR_ERR:%u", (unsigned)id);
                if (strcmp(respBuf, expected_ok)  == 0) { stm_ok = true;  got_resp = true; }
                if (strcmp(respBuf, expected_err) == 0) { stm_ok = false; got_resp = true; }
                respIdx = 0;
            } else if (c != '\r') {
                uint8_t b = (uint8_t)c;
                if (b >= 32 && b <= 126 && respIdx < 30) respBuf[respIdx++] = c;
            }
        }
        if (!got_resp) delay(50);
    }

    if (!stm_ok) {
        char msg[64];
        snprintf(msg, sizeof(msg),
                 got_resp ? "Slot %u: DeleteChar fallito (AS608)" : "Slot %u: timeout risposta STM32",
                 (unsigned)id);
        addEvent("error", msg);
        sendJson(200, "{\"ok\":false,\"as608\":false}");
        return;
    }

    if (!deleteUser(id)) {
        sendJson(500, "{\"ok\":false,\"error\":\"db_error\"}");
        return;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "Utente eliminato: %s (slot %u)", u.name, (unsigned)id);
    addEvent("db_reset", msg);
    sendJson(200, "{\"ok\":true,\"as608\":true}");
}

void handleApiReset() {
    if (!checkApiKey()) return;

    /* Svuota buffer UART in ingresso prima di inviare il comando */
    while (stm.available()) stm.read();
    stm.println("AS608_RESET");

    char     respBuf[32];
    uint8_t  respIdx  = 0;
    bool     stm_ok   = false;
    bool     got_resp = false;
    uint32_t t0       = millis();

    /* Attendi risposta STM32 per max 10 s */
    while (!got_resp && millis() - t0 < 10000) {
        while (stm.available()) {
            char c = (char)stm.read();
            if (c == '\n') {
                respBuf[respIdx] = '\0';
                if (strcmp(respBuf, "AS608_RESET_OK")  == 0) { stm_ok = true;  got_resp = true; }
                if (strcmp(respBuf, "AS608_RESET_ERR") == 0) { stm_ok = false; got_resp = true; }
                respIdx = 0;
            } else if (c != '\r') {
                uint8_t b = (uint8_t)c;
                if (b >= 32 && b <= 126 && respIdx < 30) respBuf[respIdx++] = c;
            }
        }
        if (!got_resp) delay(50);
    }

    if (!stm_ok) {
        addEvent("error", "Reset annullato: AS608 non risponde (timeout o errore sensore)");
        sendJson(200, "{\"ok\":false,\"as608\":false}");
        return;
    }

    if (!resetUserDatabase()) {
        /* AS608 azzerato ma SQLite fallito — stato già loggato da resetUserDatabase() */
        sendJson(200, "{\"ok\":false,\"as608\":true,\"error\":\"db_error\"}");
        return;
    }

    sendJson(200, "{\"ok\":true,\"as608\":true}");
}

/* ── Helpers ─────────────────────────────────────── */
String timestamp() {
    struct tm t;
    if (!getLocalTime(&t, 1000)) return "N/D";
    char buf[20];
    strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &t);
    return String(buf);
}
