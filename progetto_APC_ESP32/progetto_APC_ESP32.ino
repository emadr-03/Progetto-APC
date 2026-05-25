/* esp32_smartlock.ino — MERGED (DEBUG + v3)
   UART2: GPIO16=RX (da PB10-STM32TX), GPIO17=TX (verso PB11-STM32RX)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <time.h>
#include <LittleFS.h>
#include <sqlite3.h>
#include "esp_system.h"

/* ── DEBUG ───────────────────────────────────────── */
#define DEBUG 0

#if DEBUG
  #define DBG(x)       Serial.print(x)
  #define DBGLN(x)     Serial.println(x)
  #define DBGF(...)    Serial.printf(__VA_ARGS__)
#else
  #define DBG(x)
  #define DBGLN(x)
  #define DBGF(...)
#endif

/* ── Credenziali ─────────────────────────────────── */
const char* SSID      = "iPhone_MrDome";
const char* WIFI_PASS = "nonlasos";
const char* API_KEY   = "Pendragon";     /* X-API-KEY per le chiamate HTTP */

/* ── UART verso STM32 ────────────────────────────── */
#define STM_RX   16    /* <- PB10 (USART3_TX STM32) */
#define STM_TX   17    /* -> PB11 (USART3_RX STM32) */
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
bool    apiAuthorized();
void    startEnrollment(const char* first, const char* last);
void    onEnrollResult(const char* payload);
void    addEvent(const char* type, const char* message);
void    resetEnrollment();

/* ── Oggetti ─────────────────────────────────────── */
HardwareSerial stm(2);      /* UART2 ESP32 */
WebServer      server(80);

/* ── Buffer UART ─────────────────────────────────── */
char    rxBuf[128];
uint8_t rxIdx = 0;

/* ══════════════════════════════════════════════════ */

/* Apre (o crea) il DB e popola la tabella utenti se vuota */
void initDB() {

    DBGLN("[DB] STEP 1 - Mounting LittleFS");

    if (!LittleFS.begin(true)) {   // true = formatta se corrotto
        DBGLN("[DB] LittleFS MOUNT FAIL");
        return;
    }

    DBGLN("[DB] STEP 2 - LittleFS OK");
    DBGF("[DB] Opening DB: %s\n", DB_PATH);

    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        DBGF("[DB] Open error: %s\n", sqlite3_errmsg(db));
        return;
    }

    DBGLN("[DB] STEP 3 - SQLite OPEN OK");

    const char* createSQL =
        "CREATE TABLE IF NOT EXISTS users ("
        "  id         INTEGER PRIMARY KEY,"   /* ID AS608 (0-127) */
        "  name       TEXT    NOT NULL,"
        "  role       TEXT    NOT NULL,"
        "  has_access INTEGER NOT NULL DEFAULT 1"  /* 1=GRANTED, 0=DENIED */
        ");";

    char* errMsg = nullptr;

    DBGLN("[DB] STEP 4 - Creating table");

    if (sqlite3_exec(db, createSQL, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        DBGF("[DB] Create table error: %s\n", errMsg);
        sqlite3_free(errMsg);
        return;
    }

    DBGLN("[DB] STEP 5 - Table OK");

    const char* seedSQL =
        "INSERT OR IGNORE INTO users (id, name, role, has_access) VALUES"
        "  (1, 'Domenico', 'Proprietario', 1),"
        "  (2, 'Giovanni', 'Familiare',    1),"
        "  (3, 'Antonio',  'Ospite',       1),"
        "  (4, 'Emanuele', 'Ex-ospite',    1);";

    DBGLN("[DB] STEP 6 - Seeding");

    if (sqlite3_exec(db, seedSQL, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        DBGF("[DB] Seed error: %s\n", errMsg);
        sqlite3_free(errMsg);
    } else {
        DBGLN("[DB] STEP 7 - Seed OK / Pronto");
    }
}

User findUser(uint8_t targetId) {

    DBGF("[DB] findUser(%u)\n", targetId);

    User u = {};

    if (!db) {
        DBGLN("[DB] DB POINTER NULL");
        return u;
    }

    const char* sql =
        "SELECT id, name, role, has_access FROM users WHERE id = ? LIMIT 1;";

    sqlite3_stmt* stmt;

    DBGLN("[DB] Preparing SELECT");

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        DBGF("[DB] Prepare error: %s\n", sqlite3_errmsg(db));
        return u;
    }

    DBGLN("[DB] Binding parameter");
    sqlite3_bind_int(stmt, 1, targetId);   // binding sicuro, no SQL injection

    DBGLN("[DB] Executing SELECT");

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        DBGLN("[DB] USER FOUND");
        u.id        = (uint8_t)sqlite3_column_int(stmt, 0);
        strncpy(u.name, (const char*)sqlite3_column_text(stmt, 1), sizeof(u.name) - 1);
        strncpy(u.role, (const char*)sqlite3_column_text(stmt, 2), sizeof(u.role) - 1);
        u.hasAccess = sqlite3_column_int(stmt, 3) == 1;
        u.found     = true;
        DBGF("[DB] Name=%s Role=%s Access=%d\n", u.name, u.role, u.hasAccess);
    } else {
        DBGLN("[DB] USER NOT FOUND");
    }

    sqlite3_finalize(stmt);
    DBGLN("[DB] SELECT finalized");
    return u;
}

/* Aggiunge o aggiorna un utente */
bool update_or_insertUser(uint8_t id, const char* name, const char* role, bool access) {

    DBGF("[DB] update_or_insertUser(%u)\n", id);

    if (!db) {
        DBGLN("[DB] DB POINTER NULL");
        return false;
    }

    const char* updateSql =
        "UPDATE users SET name = ?, role = ?, has_access = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, updateSql, -1, &stmt, nullptr) != SQLITE_OK) {
        DBGF("[DB] UPDATE Prepare FAIL: %s\n", sqlite3_errmsg(db));
        return false;
    }

    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, role, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 3, access ? 1 : 0);
    sqlite3_bind_int (stmt, 4, id);

    DBGLN("[DB] Executing UPDATE");
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        DBGF("[DB] UPDATE FAIL: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return false;
    }

    int changes = sqlite3_changes(db);
    sqlite3_finalize(stmt);

    if (changes > 0) {
        DBGLN("[DB] UPDATE OK");
        return true;
    }

    const char* insertSql =
        "INSERT INTO users (id, name, role, has_access) VALUES (?,?,?,?);";

    if (sqlite3_prepare_v2(db, insertSql, -1, &stmt, nullptr) != SQLITE_OK) {
        DBGF("[DB] INSERT Prepare FAIL: %s\n", sqlite3_errmsg(db));
        return false;
    }

    sqlite3_bind_int (stmt, 1, id);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, role, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 4, access ? 1 : 0);

    DBGLN("[DB] Executing INSERT");
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) {
        DBGF("[DB] INSERT FAIL: %s\n", sqlite3_errmsg(db));
    }
    sqlite3_finalize(stmt);
    return ok;
}

/* Revoca o ripristina l'accesso senza eliminare l'utente */
bool setAccess(uint8_t id, bool access) {
    DBGF("[DB] setAccess(%u, %d)\n", id, access);

    const char* sql = "UPDATE users SET has_access = ? WHERE id = ?;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        DBGF("[DB] setAccess Prepare FAIL: %s\n", sqlite3_errmsg(db));
        return false;
    }

    sqlite3_bind_int(stmt, 1, access ? 1 : 0);
    sqlite3_bind_int(stmt, 2, id);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    DBGF("[DB] setAccess result=%d\n", ok);

    sqlite3_finalize(stmt);
    return ok;
}

/* Elimina un utente dal DB */
bool deleteUser(uint8_t id) {
    DBGF("[DB] deleteUser(%u)\n", id);

    const char* sql = "DELETE FROM users WHERE id = ?;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        DBGF("[DB] deleteUser Prepare FAIL: %s\n", sqlite3_errmsg(db));
        return false;
    }

    sqlite3_bind_int(stmt, 1, id);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    DBGF("[DB] deleteUser result=%d\n", ok);

    sqlite3_finalize(stmt);
    return ok;
}

void addEvent(const char* type, const char* message) {

    DBGF("[EVENT] %s -> %s\n", type, message);

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
    DBGLN("[ENROLL] Resetting state");
    enroll.state = ENROLL_IDLE;
    enroll.first[0] = '\0';
    enroll.last[0]  = '\0';
    enroll.id = 0;
    snprintf(enroll.message, sizeof(enroll.message), "idle");
    enroll.startedMs   = 0;
    enroll.completedMs = 0;
}

bool apiAuthorized() {

    DBGLN("[API] Checking authorization");

    if (!server.hasHeader("X-API-KEY")) {
        DBGLN("[API] Missing API KEY");
        return false;
    }

    bool ok = server.header("X-API-KEY") == API_KEY;
    DBGF("[API] Authorized=%d\n", ok);
    return ok;
}

void handleApiHealth() {

    DBGLN("[API] /health");

    if (!apiAuthorized()) {
        server.send(401, "application/json",
                    "{\"ok\":false,\"error\":\"unauthorized\"}");
        return;
    }
    server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiEvents() {

    DBGLN("[API] /events");

    if (!apiAuthorized()) {
        server.send(401, "application/json",
                    "{\"ok\":false,\"error\":\"unauthorized\"}");
        return;
    }

    uint32_t since = 0;
    if (server.hasArg("since")) {
        since = (uint32_t)atoi(server.arg("since").c_str());
        DBGF("[API] since=%u\n", since);
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

    DBGF("[ENROLL] Starting for %s %s\n", first, last);

    strncpy(enroll.first, first, sizeof(enroll.first) - 1);
    strncpy(enroll.last,  last,  sizeof(enroll.last)  - 1);
    enroll.first[sizeof(enroll.first) - 1] = '\0';
    enroll.last [sizeof(enroll.last)  - 1] = '\0';

    enroll.id          = 0;
    enroll.state       = ENROLL_IN_PROGRESS;
    enroll.completedMs = 0;

    snprintf(enroll.message, sizeof(enroll.message), "waiting_for_sensor");

    enroll.startedMs = millis();

    DBGLN("[UART TX] ENROLL_START");
    stm.println("ENROLL_START");

    char msg[128];
    snprintf(msg, sizeof(msg), "Enrollment started for %s %s",
             enroll.first, enroll.last);
    addEvent("enroll_start", msg);
}

void handleApiEnroll() {

    DBGLN("[API] /enroll");

    if (!apiAuthorized()) {
        server.send(401, "application/json",
                    "{\"ok\":false,\"error\":\"unauthorized\"}");
        return;
    }

    if (enroll.state == ENROLL_IN_PROGRESS) {
        server.send(409, "application/json",
                    "{\"ok\":false,\"error\":\"enroll_in_progress\"}");
        return;
    }

    String body = server.arg("plain");
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        server.send(400, "application/json",
                    "{\"ok\":false,\"error\":\"bad_json\"}");
        return;
    }

    const char* first = doc["first_name"] | "";
    const char* last  = doc["last_name"]  | "";
    if (strlen(first) == 0 || strlen(last) == 0) {
        server.send(400, "application/json",
                    "{\"ok\":false,\"error\":\"missing_name\"}");
        return;
    }

    startEnrollment(first, last);
    server.send(202, "application/json", "{\"ok\":true,\"status\":\"started\"}");
}

void handleApiEnrollStatus() {

    DBGLN("[API] /enroll/status");

    if (!apiAuthorized()) {
        server.send(401, "application/json",
                    "{\"ok\":false,\"error\":\"unauthorized\"}");
        return;
    }

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

    DBGLN("[HTTP] Configuring server");

    const char* headers[] = { "X-API-KEY" };
    server.collectHeaders(headers, 1);

    server.on("/api/health",        HTTP_GET,  handleApiHealth);
    server.on("/api/enroll",        HTTP_POST, handleApiEnroll);
    server.on("/api/enroll/status", HTTP_GET,  handleApiEnrollStatus);
    server.on("/api/events",        HTTP_GET,  handleApiEvents);

    server.begin();

    DBGLN("[HTTP] Server STARTED");
}

void onEnrollResult(const char* payload) {

    DBGF("[ENROLL] Result payload: %s\n", payload);

    if (strncmp(payload, "ERR:", 4) == 0) {
        DBGLN("[ENROLL] ERROR RESULT");
        enroll.state = ENROLL_ERROR;
        snprintf(enroll.message, sizeof(enroll.message), "%s", payload + 4);
        enroll.completedMs = millis();
        char msg[128];
        snprintf(msg, sizeof(msg), "Enrollment failed: %s", enroll.message);
        addEvent("enroll_error", msg);
        return;
    }

    const char* colon = strchr(payload, ':');
    if (!colon) {
        DBGLN("[ENROLL] INVALID FORMAT");
        enroll.state = ENROLL_ERROR;
        snprintf(enroll.message, sizeof(enroll.message), "bad_result");
        enroll.completedMs = millis();
        addEvent("enroll_error", "Enrollment failed: bad_result");
        return;
    }

    uint8_t     id     = (uint8_t)atoi(payload);
    const char* status = colon + 1;

    DBGF("[ENROLL] ID=%u STATUS=%s\n", id, status);

    if (strcmp(status, "OK") != 0) {
        DBGLN("[ENROLL] STATUS NOT OK");
        enroll.state = ENROLL_ERROR;
        snprintf(enroll.message, sizeof(enroll.message), "status_%s", status);
        enroll.completedMs = millis();
        char msg[128];
        snprintf(msg, sizeof(msg), "Enrollment failed: %s", enroll.message);
        addEvent("enroll_error", msg);
        return;
    }

    char fullName[64];
    snprintf(fullName, sizeof(fullName), "%s %s", enroll.first, enroll.last);

    DBGLN("[ENROLL] Writing user to DB");

    if (!update_or_insertUser(id, fullName, "User", true)) {
        DBGLN("[ENROLL] DB WRITE FAILED");
        enroll.state = ENROLL_ERROR;
        const char* err = db ? sqlite3_errmsg(db) : "db_null";
        snprintf(enroll.message, sizeof(enroll.message), "db_write_failed:%s", err);
        enroll.completedMs = millis();
        char msg[128];
        snprintf(msg, sizeof(msg), "Enrollment failed: db_write_failed (%s)", err);
        addEvent("enroll_error", msg);
        return;
    }

    enroll.id          = id;
    enroll.state       = ENROLL_DONE;
    enroll.completedMs = millis();
    snprintf(enroll.message, sizeof(enroll.message), "saved");

    DBGLN("[ENROLL] COMPLETED");

    char msg[128];
    snprintf(msg, sizeof(msg), "Enrollment ok: %s (ID %u)", fullName, (unsigned)id);
    addEvent("enroll_ok", msg);
}

void setup() {

    Serial.begin(115200);

    DBGLN("\n[BOOT] STEP 1 - Serial started");
    DBGLN("[BOOT] STEP 2 - Starting STM UART");

    stm.begin(STM_BAUD, SERIAL_8N1, STM_RX, STM_TX);

    DBGLN("[BOOT] STEP 3 - STM UART OK");
    DBGLN("[BOOT] STEP 4 - Connecting WiFi");

    WiFi.begin(SSID, WIFI_PASS);
    DBG("WiFi");
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
        DBG(".");
    }
    DBGLN("");

    if (WiFi.status() == WL_CONNECTED) {
        DBGLN("[BOOT] WiFi CONNECTED");
        DBGF("[BOOT] IP=%s\n", WiFi.localIP().toString().c_str());
    } else {
        DBGLN("[BOOT] WiFi FAILED");
    }

    DBGLN("[BOOT] STEP 5 - Configuring NTP");

    /* NTP Italia - CEST (ora legale) UTC+2 */
    configTime(3600, 3600, "pool.ntp.org");

    DBGLN("[BOOT] STEP 6 - NTP OK");
    DBGLN("[BOOT] STEP 7 - initDB");

    initDB();

    DBGLN("[BOOT] STEP 8 - setupServer");

    setupServer();

    DBGLN("[BOOT] STEP 9 - Counting users");

    int userCount = 0;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM users;", -1, &stmt, nullptr) == SQLITE_OK) {
        DBGLN("[BOOT] COUNT prepared");
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            userCount = sqlite3_column_int(stmt, 0);
            DBGF("[BOOT] Users=%d\n", userCount);
        } else {
            DBGLN("[BOOT] COUNT step FAILED");
        }
        sqlite3_finalize(stmt);
    } else {
        DBGF("[BOOT] COUNT prepare FAILED: %s\n", sqlite3_errmsg(db));
    }

    {
        char msg[128];
        snprintf(msg, sizeof(msg), "Smart Lock Online - IP %s - Users %d",
                 WiFi.localIP().toString().c_str(), userCount);
        addEvent("system", msg);
    }

    DBGLN("[BOOT] SETUP COMPLETED");
}

void loop() {

    static uint32_t lastBeat = 0;
    if (millis() - lastBeat > 5000) {
        DBGLN("[LOOP] alive");
        DBGF("[HEAP] Free heap: %u\n", ESP.getFreeHeap());
        lastBeat = millis();
    }

    while (stm.available()) {
        char c = (char)stm.read();

        DBGF("[UART RX CHAR] %c (0x%02X)\n", c, c);

        if (c == '\n') {
            rxBuf[rxIdx] = '\0';
            DBGF("[UART RX MSG] %s\n", rxBuf);
            if (rxIdx > 0) handle(rxBuf);
            rxIdx = 0;
        } else if (c != '\r' && rxIdx < 126) {
            rxBuf[rxIdx++] = c;
        }
    }

    server.handleClient();

    /* Timeout enrollment */
    if (enroll.state == ENROLL_IN_PROGRESS &&
        (millis() - enroll.startedMs > 120000)) {
        DBGLN("[ENROLL] TIMEOUT");
        enroll.state = ENROLL_ERROR;
        snprintf(enroll.message, sizeof(enroll.message), "timeout");
        enroll.completedMs = millis();
        addEvent("enroll_error", "Enrollment failed: timeout");
    }

    /* Auto-reset dopo completamento/errore */
    if ((enroll.state == ENROLL_ERROR || enroll.state == ENROLL_DONE) &&
        enroll.completedMs > 0 &&
        (millis() - enroll.completedMs > ENROLL_RESET_MS)) {
        DBGLN("[ENROLL] Auto-reset");
        resetEnrollment();
        addEvent("enroll_reset", "Enrollment state reset");
    }

    delay(10);
}

/* ── Parser comandi ──────────────────────────────── */
void handle(const char* cmd) {

    DBGF("[HANDLE] CMD=%s\n", cmd);

    /* ── ENROLL_RESULT:<id>:OK | ENROLL_RESULT:ERR:<reason> ── */
    if (strncmp(cmd, "ENROLL_RESULT:", 14) == 0) {
        DBGLN("[HANDLE] ENROLL RESULT");
        onEnrollResult(cmd + 14);
        return;
    }

    /* ── AUTH_REQUEST:<id> ── */
    if (strncmp(cmd, "AUTH_REQUEST:", 13) == 0) {
        DBGLN("[AUTH] AUTH_REQUEST");
        uint8_t reqId = (uint8_t)atoi(cmd + 13);
        DBGF("[AUTH] Requested ID=%u\n", reqId);

        User u = findUser(reqId);

        DBGF("[AUTH] found=%d access=%d name=%s\n",
             u.found, u.hasAccess, u.name);

        String resp = "AUTH_RESULT:" + String(reqId) + ":";

        if (u.found && u.hasAccess) {
            DBGLN("[AUTH] ACCESS GRANTED");
            stm.println(resp + "GRANTED");
            char msg[128];
            snprintf(msg, sizeof(msg), "Access granted: %s (ID %u)",
                     u.name, (unsigned)reqId);
            addEvent("access_granted", msg);

        } else if (u.found && !u.hasAccess) {
            DBGLN("[AUTH] ACCESS DENIED (revoked)");
            stm.println(resp + "DENIED");
            char msg[128];
            snprintf(msg, sizeof(msg), "Access denied: %s (ID %u)",
                     u.name, (unsigned)reqId);
            addEvent("access_denied", msg);

        } else {
            DBGLN("[AUTH] ACCESS DENIED (unknown ID)");
            stm.println(resp + "DENIED");
            char msg[128];
            snprintf(msg, sizeof(msg), "Unknown ID: %u", (unsigned)reqId);
            addEvent("access_unknown", msg);
        }
        return;
    }

    /* ── Impronta sconosciuta (AS608 no-match) ── */
    if (strcmp(cmd, "ALERT:UNKNOWN_FINGER") == 0) {
        DBGLN("[HANDLE] UNKNOWN FINGER");
        addEvent("finger_unknown", "Unrecognized fingerprint");
        return;
    }

    /* ── Errori generici ── */
    if (strncmp(cmd, "ERR:", 4) == 0) {
        DBGF("[HANDLE] ERROR=%s\n", cmd + 4);
        addEvent("error", cmd + 4);
        return;
    }

    /* BOOT:SYSTEM_OK -- ignorato */
    DBGLN("[HANDLE] UNKNOWN COMMAND");
}

/* ── Helpers ─────────────────────────────────────── */
String timestamp() {
    struct tm t;
    if (!getLocalTime(&t, 1000)) return "N/D";
    char buf[20];
    strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &t);
    return String(buf);
}
