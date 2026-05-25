/* ════════════════════════════════════════════════════════
   as608.h — Driver AS608 TZT · STM32F3Discovery
   USART2: PA2=TX  PA3=RX  57600 baud
   TCH  : PC5 — EXTI5 rising edge (rilevamento dito)
   ════════════════════════════════════════════════════════ */
#ifndef AS608_H
#define AS608_H

#include "main.h"   /* include HAL + pin definitions del progetto */

/* ── Indirizzo di Default del Modulo ────────────────────── */
#define AS608_DEFAULT_ADDR    0xFFFFFFFF

/* ── Identificatori del Tipo di Pacchetto (PID) ─────────── */
#define AS608_PID_COMMAND     0x01  /* Pacchetto Comando  (STM32 → AS608) */
#define AS608_PID_DATA        0x02  /* Pacchetto Dati                      */
#define AS608_PID_ACK         0x07  /* Pacchetto Risposta (AS608 → STM32) */
#define AS608_PID_END         0x08  /* Fine pacchetto dati                 */

/* ── Set di Comandi Principali (Instruction Codes) ──────── */
#define AS608_CMD_GET_IMAGE   0x01  /* Scansiona il dito                   */
#define AS608_CMD_GEN_CHAR    0x02  /* Genera template dall'immagine       */
#define AS608_CMD_MATCH       0x03  /* Confronta due template in RAM       */
#define AS608_CMD_SEARCH      0x04  /* Cerca impronta nel database         */
#define AS608_CMD_REG_MODEL   0x05  /* Unisci template per creare modello  */
#define AS608_CMD_STORE_CHAR  0x06  /* Salva modello nel database          */
#define AS608_CMD_LOAD_CHAR   0x07  /* Carica modello dal database in RAM  */
#define AS608_CMD_DELETE_CHAR 0x0C  /* Cancella uno o più slot             */
#define AS608_CMD_EMPTY_DB    0x0D  /* Svuota l'intero database            */
#define AS608_CMD_SYS_PARAM   0x0F  /* Leggi parametri di sistema          */
#define AS608_CMD_VFY_PWD     0x13  /* Verifica Password (Handshake)       */

/* ── Codici di Ritorno (Confirmation Codes) ─────────────── */
#define AS608_OK              0x00  /* Esecuzione completata con successo  */
#define AS608_ERR_COMM        0x01  /* Errore di ricezione pacchetto       */
#define AS608_ERR_NO_FINGER   0x02  /* Nessun dito sul sensore             */
#define AS608_ERR_IMG_FAIL    0x03  /* Impossibile acquisire l'immagine    */
#define AS608_ERR_DB_FULL     0x0B  /* Database impronte pieno             */
#define AS608_ERR_PWD_WRONG   0x13  /* Password errata                     */
#define AS608_ERR_TIMEOUT     0xFF  /* Errore Custom: Timeout Seriale      */

/*
 * AS608_Result — alias uint8_t.
 * Le funzioni restituiscono direttamente il Confirmation Code del modulo
 * (o AS608_ERR_TIMEOUT in caso di timeout seriale).
 * Confronta sempre con le costanti AS608_OK / AS608_ERR_* qui sopra.
 */
typedef uint8_t AS608_Result;

/* ── Identificatori CharBuffer ───────────────────────────── */
#define AS608_CHARBUF_1       0x01   /* CharBuffer 1 — prima acquisizione  */
#define AS608_CHARBUF_2       0x02   /* CharBuffer 2 — seconda acquisizione */

/* ── API pubblica ────────────────────────────────────────── */
void         AS608_Init(UART_HandleTypeDef* h);

/*
 * AS608_IsFingerPresent()
 * Metodo di polling opzionale: il rilevamento primario avviene
 * via interrupt EXTI5 sul pin TCH (PC5).
 * Ritorna 1 se un dito è presente, 0 altrimenti.
 */
uint8_t      AS608_IsFingerPresent(void);

/* Handshake iniziale — da chiamare a ogni boot prima di qualsiasi operazione */
AS608_Result AS608_VerifyPassword(void);

/* Cancella tutti i template dalla libreria flash del modulo */
AS608_Result AS608_EmptyDB(void);

/*
 * AS608_DeleteChar — cancella un singolo slot dalla libreria flash.
 * Più affidabile di EmptyDB su alcune varianti del modulo.
 * slot: indice da cancellare (0-127)
 */
AS608_Result AS608_DeleteChar(uint8_t slot);

/* Identificazione 1:N — cerca il dito nell'intera libreria */
AS608_Result AS608_IdentifyFinger(uint8_t* id, uint16_t* score);

/*
 * Funzioni atomiche per enrollment guidato.
 * Ordine di chiamata consigliato:
 *   GetImage → GenChar(CHARBUF_1) → [rimuovi dito]
 *   → GetImage → GenChar(CHARBUF_2) → RegModel → StoreChar(slot)
 */
AS608_Result AS608_GetImage(void);
AS608_Result AS608_GenChar(uint8_t charBufID);
AS608_Result AS608_RegModel(void);
AS608_Result AS608_StoreChar(uint8_t slot);

/*
 * AS608_LoadChar — tenta di caricare il template dello slot in charBufID.
 * Ritorna AS608_OK se lo slot contiene dati, errore altrimenti.
 * Usata internamente da AS608_FindFreeSlot.
 */
AS608_Result AS608_LoadChar(uint8_t charBufID, uint8_t slot);

/*
 * AS608_FindFreeSlot — scansiona la libreria (slot 1-127) con LoadChar
 * e restituisce il primo slot vuoto. Ritorna -1 se il DB è pieno.
 * Parte da slot 1 per evitare l'ID 0 che l'ESP32 non gestisce.
 */
int8_t       AS608_FindFreeSlot(void);

/* Wrapper ad alto livello */
AS608_Result AS608_EnrollFinger(uint8_t id);

#endif /* AS608_H */
