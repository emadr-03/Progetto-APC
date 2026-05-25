/* esp32_smartlock.ino — v3
   UART2: GPIO16=RX (da PB10-STM32TX), GPIO17=TX (verso PB11-STM32RX)
   Database utenti in array statico (espandibile con NVS/SPIFFS).    */

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

sqlite3* db =nullptr;
const char* DB_PATH="/littlefs/smartlock.db";

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

#define ENROLL_RESET_MS 5000

EnrollContext enroll = { ENROLL_IDLE, "", "", 0, "idle", 0, 0 };

struct EventLog {
    uint32_t id;
    char ts[20];
    char type[20];
    char message[120];
};

#define MAX_EVENTS 30
EventLog events[MAX_EVENTS];
uint8_t eventSize = 0;
uint8_t eventHead = 0;
uint32_t nextEventId = 1;

/* ── Prototipi ───────────────────────────────────── */
void handle(const char* cmd);
String timestamp();
void setupServer();
void handleApiEnroll();
void handleApiEnrollStatus();
void handleApiHealth();
void handleApiEvents();
bool apiAuthorized();
void startEnrollment(const char* first, const char* last);
void onEnrollResult(const char* payload);
void addEvent(const char* type, const char* message);
void resetEnrollment();

/* ── Oggetti ─────────────────────────────────────── */
HardwareSerial    stm(2);   /* UART2 ESP32 */
WebServer         server(80);

/* ── Buffer UART ─────────────────────────────────── */
char    rxBuf[128];
uint8_t rxIdx = 0;

/* ══════════════════════════════════════════════════ */

/* Apre (o crea) il DB e popola la tabella utenti se vuota */
void initDB() {
    if (!LittleFS.begin(true)) {   // true = formatta se corrotto
        Serial.println("[DB] LittleFS mount FAIL");
        return;
    }

    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        Serial.printf("[DB] Open error: %s\n", sqlite3_errmsg(db));
        return;
    }

    /* Crea tabella se non esiste */
    const char* createSQL =
        "CREATE TABLE IF NOT EXISTS users ("
        "  id         INTEGER PRIMARY KEY,"   /* ID AS608 (0-127) */
        "  name       TEXT    NOT NULL,"
        "  role       TEXT    NOT NULL,"
        "  has_access INTEGER NOT NULL DEFAULT 1"  /* 1=GRANTED, 0=DENIED */
        ");";

    char* errMsg = nullptr;
    if (sqlite3_exec(db, createSQL, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        Serial.printf("[DB] Create table error: %s\n", errMsg);
        sqlite3_free(errMsg);
        return;
    }

    /* Seed iniziale — inserisce solo se la tabella è vuota */
    const char* seedSQL =
        "INSERT OR IGNORE INTO users (id, name, role, has_access) VALUES"
        "  (1, 'Domenico',  'Proprietario', 1),"
        "  (2, 'Giovanni', 'Familiare',    1),"
        "  (3, 'Antonio',  'Ospite',       1),"
        "  (4, 'Emanuele',     'Ex-ospite',    1);";

    if (sqlite3_exec(db, seedSQL, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        Serial.printf("[DB] Seed error: %s\n", errMsg);
        sqlite3_free(errMsg);
    } else {
        Serial.println("[DB] Pronto");
    }
}

User findUser(uint8_t targetId) {
    User u = {};
    if (!db) return u;

    const char* sql = "SELECT id, name, role, has_access FROM users WHERE id = ? LIMIT 1;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        Serial.printf("[DB] Prepare error: %s\n", sqlite3_errmsg(db));
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
    const char* sql =
        "INSERT INTO users (id, name, role, has_access) VALUES (?,?,?,?)"
        " ON CONFLICT(id) DO UPDATE SET name=excluded.name,"
        "   role=excluded.role, has_access=excluded.has_access;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_int (stmt, 1, id);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, role, -1, SQLITE_STATIC);
    sqlite3_bind_int (stmt, 4, access ? 1 : 0);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

/* Revoca o ripristina l'accesso senza eliminare l'utente */
bool setAccess(uint8_t id, bool access) {
    const char* sql = "UPDATE users SET has_access = ? WHERE id = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, access ? 1 : 0);
    sqlite3_bind_int(stmt, 2, id);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

/* Elimina un utente dal DB */
bool deleteUser(uint8_t id) {
    const char* sql = "DELETE FROM users WHERE id = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

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
    enroll.last[0] = '\0';
    enroll.id = 0;
    snprintf(enroll.message, sizeof(enroll.message), "idle");
    enroll.startedMs = 0;
    enroll.completedMs = 0;
}

bool apiAuthorized() {
    if (!server.hasHeader("X-API-KEY")) return false;
    return server.header("X-API-KEY") == API_KEY;
}

void handleApiHealth() {
    if (!apiAuthorized()) {
        server.send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
        return;
    }
    server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiEvents() {
    if (!apiAuthorized()) {
        server.send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
        return;
    }

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
        item["id"] = e->id;
        item["ts"] = e->ts;
        item["type"] = e->type;
        item["message"] = e->message;
    }

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

void startEnrollment(const char* first, const char* last) {
    strncpy(enroll.first, first, sizeof(enroll.first) - 1);
    strncpy(enroll.last, last, sizeof(enroll.last) - 1);
    enroll.first[sizeof(enroll.first) - 1] = '\0';
    enroll.last[sizeof(enroll.last) - 1] = '\0';
    enroll.id = 0;
    enroll.state = ENROLL_IN_PROGRESS;
    snprintf(enroll.message, sizeof(enroll.message), "waiting_for_sensor");
    enroll.startedMs = millis();
    enroll.completedMs = 0;
    stm.println("ENROLL_START");

    char msg[128];
    snprintf(msg, sizeof(msg), "Enrollment started for %s %s", enroll.first, enroll.last);
    addEvent("enroll_start", msg);
}

void handleApiEnroll() {
    if (!apiAuthorized()) {
        server.send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
        return;
    }

    if (enroll.state == ENROLL_IN_PROGRESS) {
        server.send(409, "application/json", "{\"ok\":false,\"error\":\"enroll_in_progress\"}");
        return;
    }

    String body = server.arg("plain");
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad_json\"}");
        return;
    }

    const char* first = doc["first_name"] | "";
    const char* last  = doc["last_name"] | "";
    if (strlen(first) == 0 || strlen(last) == 0) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing_name\"}");
        return;
    }

    startEnrollment(first, last);
    server.send(202, "application/json", "{\"ok\":true,\"status\":\"started\"}");
}

void handleApiEnrollStatus() {
    if (!apiAuthorized()) {
        server.send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
        return;
    }

    StaticJsonDocument<256> doc;
    doc["ok"] = true;

    switch (enroll.state) {
        case ENROLL_IDLE:        doc["status"] = "idle"; break;
        case ENROLL_IN_PROGRESS: doc["status"] = "in_progress"; break;
        case ENROLL_DONE:        doc["status"] = "done"; break;
        case ENROLL_ERROR:       doc["status"] = "error"; break;
        default:                 doc["status"] = "unknown"; break;
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

    server.on("/api/health", HTTP_GET, handleApiHealth);
    server.on("/api/enroll", HTTP_POST, handleApiEnroll);
    server.on("/api/enroll/status", HTTP_GET, handleApiEnrollStatus);
    server.on("/api/events", HTTP_GET, handleApiEvents);
    server.begin();
}

void onEnrollResult(const char* payload) {
    if (strncmp(payload, "ERR:", 4) == 0) {
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
        enroll.state = ENROLL_ERROR;
        snprintf(enroll.message, sizeof(enroll.message), "bad_result");
        enroll.completedMs = millis();
        addEvent("enroll_error", "Enrollment failed: bad_result");
        return;
    }

    uint8_t id = (uint8_t)atoi(payload);
    const char* status = colon + 1;
    if (strcmp(status, "OK") != 0) {
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

    if (!update_or_insertUser(id, fullName, "User", true)) {
        enroll.state = ENROLL_ERROR;
        snprintf(enroll.message, sizeof(enroll.message), "db_write_failed");
        enroll.completedMs = millis();
        addEvent("enroll_error", "Enrollment failed: db_write_failed");
        return;
    }

    enroll.id = id;
    enroll.state = ENROLL_DONE;
    snprintf(enroll.message, sizeof(enroll.message), "saved");
    enroll.completedMs = millis();

    char msg[128];
    snprintf(msg, sizeof(msg), "Enrollment ok: %s (ID %u)", fullName, (unsigned)id);
    addEvent("enroll_ok", msg);
}

void setup() {
    Serial.begin(115200);
    stm.begin(STM_BAUD, SERIAL_8N1, STM_RX, STM_TX);

    WiFi.begin(SSID, WIFI_PASS);
    Serial.print("WiFi");
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
        Serial.print('.');
    }
    Serial.println(WiFi.status() == WL_CONNECTED ? " OK" : " FAIL");
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("ESP32 IP: ");
        Serial.println(WiFi.localIP());
    }

    /* NTP Italia - CEST (ora legale) UTC+2 */
    configTime(3600, 3600, "pool.ntp.org");

    initDB();

    setupServer();

    // Conta utenti dal DB
    int userCount = 0;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM users;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            userCount = sqlite3_column_int(stmt, 0);
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
            rxBuf[rxIdx] = '\0';
            if (rxIdx > 0) handle(rxBuf);
            rxIdx = 0;
        } else if (c != '\r' && rxIdx < 126) {
            rxBuf[rxIdx++] = c;
        }
    }
    server.handleClient();
    if (enroll.state == ENROLL_IN_PROGRESS && (millis() - enroll.startedMs > 120000)) {
        enroll.state = ENROLL_ERROR;
        snprintf(enroll.message, sizeof(enroll.message), "timeout");
        enroll.completedMs = millis();
        addEvent("enroll_error", "Enrollment failed: timeout");
    }
    if ((enroll.state == ENROLL_ERROR || enroll.state == ENROLL_DONE) && enroll.completedMs > 0) {
        if (millis() - enroll.completedMs > ENROLL_RESET_MS) {
            resetEnrollment();
            addEvent("enroll_reset", "Enrollment state reset");
        }
    }
    delay(10);
}

/* ── Parser comandi ──────────────────────────────── */
void handle(const char* cmd) {
    Serial.printf("[STM32->] %s\n", cmd);

    /* ── ENROLL_RESULT:<id>:OK | ENROLL_RESULT:ERR:<reason> ── */
    if (strncmp(cmd, "ENROLL_RESULT:", 14) == 0) {
        onEnrollResult(cmd + 14);
        return;
    }

    /* ── AUTH_REQUEST:<id> ── */
if (strncmp(cmd, "AUTH_REQUEST:", 13) == 0) {
    uint8_t reqId = (uint8_t)atoi(cmd + 13);
    User u = findUser(reqId);          // ← User per valore, non puntatore
    String resp = "AUTH_RESULT:" + String(reqId) + ":";

    if (u.found && u.hasAccess) {
        stm.println(resp + "GRANTED");
        char msg[128];
        snprintf(msg, sizeof(msg), "Access granted: %s (ID %u)", u.name, (unsigned)reqId);
        addEvent("access_granted", msg);
    }
    else if (u.found && !u.hasAccess) {
        stm.println(resp + "DENIED");
        char msg[128];
        snprintf(msg, sizeof(msg), "Access denied: %s (ID %u)", u.name, (unsigned)reqId);
        addEvent("access_denied", msg);
    }
    else {
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
    /* BOOT:SYSTEM_OK -- ignorato qui */
}

/* ── Helpers ─────────────────────────────────────── */
String timestamp() {
    struct tm t;
    if (!getLocalTime(&t, 1000)) return "N/D";
    char buf[20];
    strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &t);
    return String(buf);
}
