/* ════════════════════════════════════════════════════════
   as608.c — Implementazione driver AS608 TZT
   ════════════════════════════════════════════════════════ */
#include "as608.h"

#define AS608_RX_TIMEOUT_MS   3000   /* timeout HAL_UART_Receive          */
#define AS608_HEADER_0        0xEF   /* magic byte 0 del frame AS608      */
#define AS608_HEADER_1        0x01   /* magic byte 1 del frame AS608      */
#define AS608_LIB_START_HI    0x00   /* inizio ricerca libreria — MSB     */
#define AS608_LIB_START_LO    0x00   /* inizio ricerca libreria — LSB     */
#define AS608_LIB_END_HI      0x00   /* fine ricerca — MSB (127 slot)     */
#define AS608_LIB_END_LO      0x7F   /* fine ricerca — LSB                */

static UART_HandleTypeDef* _huart;

/* ── Funzione interna: costruisce e trasmette un pacchetto ─
 *
 * Struttura frame AS608:
 *  [0-1]  Header       : 0xEF 0x01
 *  [2-5]  Indirizzo    : 4 byte (MSB first) — AS608_DEFAULT_ADDR
 *  [6]    PID          : tipo pacchetto
 *  [7-8]  Lunghezza    : len(payload) + 2 (include i 2 byte checksum)
 *  [9..N] Payload      : dati / comando
 *  [N+1-N+2] Checksum : somma di PID + Length + Payload (uint16, MSB)
 */
static void AS608_SendPacket(uint8_t pid, const uint8_t* payload, uint16_t plen) {
    uint8_t  frame[40];
    uint16_t idx  = 0;
    uint16_t flen = plen + 2;                        /* payload + checksum */
    uint16_t chk  = pid
                  + (uint8_t)(flen >> 8)
                  + (uint8_t)(flen & 0xFF);

    /* Header */
    frame[idx++] = AS608_HEADER_0;
    frame[idx++] = AS608_HEADER_1;

    /* Indirizzo modulo (4 byte, MSB first) */
    frame[idx++] = (uint8_t)((AS608_DEFAULT_ADDR >> 24) & 0xFF);
    frame[idx++] = (uint8_t)((AS608_DEFAULT_ADDR >> 16) & 0xFF);
    frame[idx++] = (uint8_t)((AS608_DEFAULT_ADDR >>  8) & 0xFF);
    frame[idx++] = (uint8_t)((AS608_DEFAULT_ADDR      ) & 0xFF);

    /* PID + lunghezza */
    frame[idx++] = pid;
    frame[idx++] = (uint8_t)(flen >> 8);
    frame[idx++] = (uint8_t)(flen & 0xFF);

    /* Payload + accumulo checksum */
    for (uint16_t j = 0; j < plen; j++) {
        frame[idx++] = payload[j];
        chk += payload[j];
    }

    /* Checksum (2 byte, MSB first) */
    frame[idx++] = (uint8_t)(chk >> 8);
    frame[idx++] = (uint8_t)(chk & 0xFF);

    HAL_UART_Transmit(_huart, frame, idx, 500);
}

/* ── Funzione interna: riceve ACK e restituisce Confirmation Code ─
 *
 * Frame di risposta (minimo 12 byte):
 *  [0-1]  Header
 *  [2-5]  Indirizzo
 *  [6]    PID  (atteso: AS608_PID_ACK = 0x07)
 *  [7-8]  Lunghezza
 *  [9]    Confirmation Code  ← valore di ritorno
 *  [10..] Dati aggiuntivi (dipende dal comando)
 *  [N-1,N] Checksum
 */
static uint8_t AS608_RecvAck(uint8_t* rxBuf, uint16_t rxLen) {
    if (HAL_UART_Receive(_huart, rxBuf, rxLen, AS608_RX_TIMEOUT_MS) != HAL_OK)
        return AS608_ERR_TIMEOUT;
    return rxBuf[9];   /* Confirmation Code */
}

/* ════════════════════════════════════════════════════════
   API pubblica
   ════════════════════════════════════════════════════════ */

void AS608_Init(UART_HandleTypeDef* h) {
    _huart = h;
}

/* ── AS608_IsFingerPresent ───────────────────────────────
 * Invia AS608_CMD_GET_IMAGE e interpreta il Confirmation Code:
 *   AS608_OK (0x00)             → dito presente, immagine acquisita → 1
 *   AS608_ERR_NO_FINGER (0x02)  → sensore vuoto                    → 0
 *   qualsiasi altro codice      → errore / timeout                  → 0
 */
uint8_t AS608_IsFingerPresent(void) {
    uint8_t cmd[]    = { AS608_CMD_GET_IMAGE };
    uint8_t resp[12] = {0};
    AS608_SendPacket(AS608_PID_COMMAND, cmd, sizeof(cmd));
    uint8_t cc = AS608_RecvAck(resp, sizeof(resp));
    return (cc == AS608_OK) ? 1 : 0;
}

/* ── AS608_VerifyPassword ────────────────────────────────
 * Handshake iniziale obbligatorio.
 * Invia la password di default (0x00000000).
 * Ritorna AS608_OK se il modulo risponde correttamente.
 */
AS608_Result AS608_VerifyPassword(void) {
    uint8_t cmd[] = {
        AS608_CMD_VFY_PWD,
        0x00, 0x00, 0x00, 0x00   /* password default */
    };
    uint8_t resp[12] = {0};
    AS608_SendPacket(AS608_PID_COMMAND, cmd, sizeof(cmd));
    return AS608_RecvAck(resp, sizeof(resp));
}

/* ── AS608_EmptyDB ───────────────────────────────────────
 * Cancella tutti i template dalla libreria del modulo.
 * Comando 0x0D (variante standard della maggior parte dei moduli).
 * Se non funziona usa AS608_DeleteChar slot per slot come fallback.
 */
AS608_Result AS608_EmptyDB(void) {
    uint8_t cmd[]    = { AS608_CMD_EMPTY_DB };
    uint8_t resp[12] = {0};
    AS608_SendPacket(AS608_PID_COMMAND, cmd, sizeof(cmd));
    return AS608_RecvAck(resp, sizeof(resp));
}

/* ── AS608_DeleteChar ────────────────────────────────────
 * Cancella un singolo slot dalla libreria flash.
 * Più affidabile di EmptyDB su alcune varianti del modulo.
 * slot: indice da cancellare (0-127)
 */
AS608_Result AS608_DeleteChar(uint8_t slot) {
    uint8_t cmd[] = {
        AS608_CMD_DELETE_CHAR,
        0x00, slot,  /* slot MSB + LSB */
        0x00, 0x01   /* count: cancella 1 slot */
    };
    uint8_t resp[12] = {0};
    AS608_SendPacket(AS608_PID_COMMAND, cmd, sizeof(cmd));
    return AS608_RecvAck(resp, sizeof(resp));
}

/* ── AS608_IdentifyFinger ────────────────────────────────
 * Flusso:
 *   1. GET_IMAGE  — acquisisce immagine dal sensore
 *   2. GEN_CHAR   — converte immagine in template (CharBuffer1)
 *   3. SEARCH     — cerca CharBuffer1 nell'intera libreria
 *
 * Output (se AS608_OK):
 *   *id    = slot AS608 dell'impronta riconosciuta (0-127)
 *   *score = punteggio di corrispondenza (più alto = migliore)
 */
AS608_Result AS608_IdentifyFinger(uint8_t* id, uint16_t* score) {
    uint8_t resp[16] = {0};
    uint8_t cc;

    /* Passo 1 — Acquisisci immagine */
    {
        uint8_t cmd[] = { AS608_CMD_GET_IMAGE };
        AS608_SendPacket(AS608_PID_COMMAND, cmd, sizeof(cmd));
        cc = AS608_RecvAck(resp, 12);
        if (cc != AS608_OK) return cc;
    }

    /* Passo 2 — Genera template in CharBuffer1 */
    {
        uint8_t cmd[] = { AS608_CMD_GEN_CHAR, AS608_CHARBUF_1 };
        AS608_SendPacket(AS608_PID_COMMAND, cmd, sizeof(cmd));
        cc = AS608_RecvAck(resp, 12);
        if (cc != AS608_OK) return AS608_ERR_IMG_FAIL;
    }

    /* Passo 3 — Cerca CharBuffer1 nella libreria (slot 0..127) */
    {
        uint8_t cmd[] = {
            AS608_CMD_SEARCH,
            AS608_CHARBUF_1,
            AS608_LIB_START_HI, AS608_LIB_START_LO,
            AS608_LIB_END_HI,   AS608_LIB_END_LO
        };
        AS608_SendPacket(AS608_PID_COMMAND, cmd, sizeof(cmd));
        /* Risposta SEARCH 16 byte:
           [9]     Confirmation Code
           [10-11] Page ID (slot trovato)
           [12-13] Match Score                         */
        cc = AS608_RecvAck(resp, 16);
        if (cc == AS608_OK) {
            *id    = resp[11];
            *score = ((uint16_t)resp[12] << 8) | resp[13];
        }
        return cc;
    }
}

/* ── AS608_GetImage ──────────────────────────────────────
 * Scansiona il sensore e acquisisce l'immagine del dito.
 */
AS608_Result AS608_GetImage(void) {
    uint8_t cmd[]    = { AS608_CMD_GET_IMAGE };
    uint8_t resp[12] = {0};
    AS608_SendPacket(AS608_PID_COMMAND, cmd, sizeof(cmd));
    return AS608_RecvAck(resp, sizeof(resp));
}

/* ── AS608_GenChar ───────────────────────────────────────
 * Converte l'immagine acquisita in un template nel CharBuffer specificato.
 */
AS608_Result AS608_GenChar(uint8_t charBufID) {
    uint8_t cmd[]    = { AS608_CMD_GEN_CHAR, charBufID };
    uint8_t resp[12] = {0};
    AS608_SendPacket(AS608_PID_COMMAND, cmd, sizeof(cmd));
    return AS608_RecvAck(resp, sizeof(resp));
}

/* ── AS608_RegModel ──────────────────────────────────────
 * Combina CharBuffer1 e CharBuffer2 per generare il modello definitivo.
 * Ritorna AS608_OK o 0x0A (template troppo diversi).
 */
AS608_Result AS608_RegModel(void) {
    uint8_t cmd[]    = { AS608_CMD_REG_MODEL };
    uint8_t resp[12] = {0};
    AS608_SendPacket(AS608_PID_COMMAND, cmd, sizeof(cmd));
    return AS608_RecvAck(resp, sizeof(resp));
}

/* ── AS608_StoreChar ─────────────────────────────────────
 * Salva il modello in CharBuffer1 nello slot specificato (0-127).
 */
AS608_Result AS608_StoreChar(uint8_t slot) {
    uint8_t cmd[] = {
        AS608_CMD_STORE_CHAR,
        AS608_CHARBUF_1,
        0x00,
        slot
    };
    uint8_t resp[12] = {0};
    AS608_SendPacket(AS608_PID_COMMAND, cmd, sizeof(cmd));
    return AS608_RecvAck(resp, sizeof(resp));
}

/* ── AS608_LoadChar ──────────────────────────────────────
 * Carica il template dello slot nel CharBuffer indicato.
 * Ritorna AS608_OK se lo slot contiene un template valido.
 */
AS608_Result AS608_LoadChar(uint8_t charBufID, uint8_t slot) {
    uint8_t cmd[] = {
        AS608_CMD_LOAD_CHAR,
        charBufID,
        0x00,
        slot
    };
    uint8_t resp[12] = {0};
    AS608_SendPacket(AS608_PID_COMMAND, cmd, sizeof(cmd));
    return AS608_RecvAck(resp, sizeof(resp));
}

/* ── AS608_FindFreeSlot ──────────────────────────────────
 * Scansiona la libreria slot per slot partendo da 1 (non 0).
 * Slot 0 escluso: l'ESP32 non ha utenti con id=0 e causerebbe
 * falsi negativi su Telegram.
 *
 * Ritorna il primo slot libero (1-127), oppure -1 se DB pieno.
 */
int8_t AS608_FindFreeSlot(void) {
    for (uint8_t slot = 1; slot <= 127; slot++) {
        AS608_Result r = AS608_LoadChar(AS608_CHARBUF_2, slot);
        if (r != AS608_OK) {
            return (int8_t)slot;   /* slot vuoto trovato */
        }
    }
    return -1;   /* database pieno */
}

/* ── AS608_EnrollFinger ──────────────────────────────────
 * Registra una nuova impronta nello slot 'id' (1-127).
 * Flusso (doppia acquisizione):
 *   GetImage → GenChar(1) → [rimuovi dito] → GetImage → GenChar(2)
 *   → RegModel → StoreChar(id)
 */
AS608_Result AS608_EnrollFinger(uint8_t id) {
    uint8_t resp[12] = {0};
    uint8_t cc;

    for (int cap = 1; cap <= 2; cap++) {

        /* Attendi presenza dito — timeout 10 secondi */
        uint32_t t0 = HAL_GetTick();
        while (!AS608_IsFingerPresent()) {
            if (HAL_GetTick() - t0 > 10000) return AS608_ERR_TIMEOUT;
            HAL_Delay(100);
        }

        /* Acquisisci immagine */
        {
            uint8_t cmd[] = { AS608_CMD_GET_IMAGE };
            AS608_SendPacket(AS608_PID_COMMAND, cmd, sizeof(cmd));
            cc = AS608_RecvAck(resp, 12);
            if (cc != AS608_OK) return AS608_ERR_IMG_FAIL;
        }

        /* Converti in CharBuffer<cap> */
        {
            uint8_t cmd[] = { AS608_CMD_GEN_CHAR, (uint8_t)cap };
            AS608_SendPacket(AS608_PID_COMMAND, cmd, sizeof(cmd));
            cc = AS608_RecvAck(resp, 12);
            if (cc != AS608_OK) return AS608_ERR_IMG_FAIL;
        }

        /* Dopo la prima acquisizione: aspetta che il dito venga tolto */
        if (cap == 1) {
            HAL_Delay(800);
            while (AS608_IsFingerPresent()) HAL_Delay(100);
            HAL_Delay(500);
        }
    }

    /* Genera modello definitivo */
    {
        uint8_t cmd[] = { AS608_CMD_REG_MODEL };
        AS608_SendPacket(AS608_PID_COMMAND, cmd, sizeof(cmd));
        cc = AS608_RecvAck(resp, 12);
        if (cc != AS608_OK) return cc;
    }

    /* Salva il modello nello slot 'id' */
    {
        uint8_t cmd[] = {
            AS608_CMD_STORE_CHAR,
            AS608_CHARBUF_1,
            0x00,
            id
        };
        AS608_SendPacket(AS608_PID_COMMAND, cmd, sizeof(cmd));
        return AS608_RecvAck(resp, 12);
    }
}
