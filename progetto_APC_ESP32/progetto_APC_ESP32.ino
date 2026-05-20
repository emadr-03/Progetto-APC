/* esp32_smartlock.ino — v3
   UART2: GPIO16=RX (da PB10-STM32TX), GPIO17=TX (verso PB11-STM32RX)
   Database utenti in array statico (espandibile con NVS/SPIFFS).    */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <time.h>
#include <LittleFS.h>
#include <sqlite3.h>

/* ── Credenziali ─────────────────────────────────── */
const char* SSID      = "Giovanni";
const char* WIFI_PASS = "PendragonGi";
const char* BOT_TOKEN = "8549113935:AAGg_RhQcl0YDdibSQC4-ZkMCOxJlzwAgKc";     /* @BotFather */
const char* CHAT_ID   = "218264258";    /* @userinfobot */

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

/* ── Prototipi ───────────────────────────────────── */
void handle(const char* cmd);
void tg(String text);
String timestamp();

/* ── Oggetti ─────────────────────────────────────── */
WiFiClientSecure  client;
UniversalTelegramBot bot(BOT_TOKEN, client);
HardwareSerial    stm(2);   /* UART2 ESP32 */

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
        "  (1, 'Mario Rossi',  'Proprietario', 1),"
        "  (2, 'Anna Bianchi', 'Familiare',    1),"
        "  (3, 'Luigi Verdi',  'Ospite',       1),"
        "  (4, 'Revocato',     'Ex-ospite',    0);";

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

    /* NTP Italia - CEST (ora legale) UTC+2 */
    configTime(3600, 3600, "pool.ntp.org");
    client.setInsecure();

    initDB();

    // Conta utenti dal DB
    int userCount = 0;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM users;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            userCount = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }  

    tg(String("\xF0\x9F\x9F\xA2") + " *Smart Lock Online*\n"
       "IP: " + WiFi.localIP().toString() + "\n"
       "Utenti in DB: " + String(userCount));

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
    delay(10);
}

/* ── Parser comandi ──────────────────────────────── */
void handle(const char* cmd) {
    Serial.printf("[STM32->] %s\n", cmd);
    String ts = timestamp();

    /* ── AUTH_REQUEST:<id> ── */
if (strncmp(cmd, "AUTH_REQUEST:", 13) == 0) {
    uint8_t reqId = (uint8_t)atoi(cmd + 13);
    User u = findUser(reqId);          // ← User per valore, non puntatore
    String resp = "AUTH_RESULT:" + String(reqId) + ":";

    if (u.found && u.hasAccess) {
        stm.println(resp + "GRANTED");
        tg(String("\xE2\x9C\x85") + " *Accesso Autorizzato*\n"
           "\xF0\x9F\x91\xA4 " + String(u.name) + "\n"
           "\xF0\x9F\x94\x96 " + String(u.role) + "\n"
           "\xF0\x9F\x95\x90 " + timestamp());
    }
    else if (u.found && !u.hasAccess) {
        stm.println(resp + "DENIED");
        tg(String("\xE2\x9B\x94") + " *Accesso Revocato*\n"
           "\xF0\x9F\x91\xA4 " + String(u.name) + " (ID " + String(reqId) + ")\n"
           "\xF0\x9F\x95\x90 " + timestamp());
    }
    else {
        stm.println(resp + "DENIED");
        tg(String("\xE2\x9A\xA0\xEF\xB8\x8F") + " *ID sconosciuto*\n"
           "\xF0\x9F\x94\xA2 ID: " + String(reqId) + "\n"
           "\xF0\x9F\x95\x90 " + timestamp());
    }
    return;
}

    /* ── Impronta sconosciuta (AS608 no-match) ── */
    if (strcmp(cmd, "ALERT:UNKNOWN_FINGER") == 0) {
        tg(String("\xF0\x9F\x9A\xA8") + " *Tentativo di Accesso Non Autorizzato*\n"
           "\xE2\x9D\x8C Impronta non riconosciuta\n"
           "\xF0\x9F\x94\x92 Accesso bloccato\n"
           "\xF0\x9F\x95\x90 " + ts);
        return;
    }

    /* ── Errori generici ── */
    if (strncmp(cmd, "ERR:", 4) == 0) {
        tg(String("\xF0\x9F\x94\xB4") + " *Errore sistema*\n" +
           String(cmd + 4) + "\n"
           "\xF0\x9F\x95\x90 " + ts);
        return;
    }
    /* BOOT:SYSTEM_OK -- ignorato qui */
}

/* ── Helpers ─────────────────────────────────────── */
void tg(String text) {
    if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); delay(3000); }
    if (!bot.sendMessage(CHAT_ID, text, "Markdown"))
        { delay(1000); bot.sendMessage(CHAT_ID, text, "Markdown"); }
}

String timestamp() {
    struct tm t;
    if (!getLocalTime(&t, 1000)) return "N/D";
    char buf[20];
    strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &t);
    return String(buf);
}
