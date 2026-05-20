/*
 * lcd.c
 *
 *  Created on: May 15, 2026
 *      Author: dome-
 */

/* lcd1602.c — Implementazione 4-bit write-only (RW=GND) */
#include "lcd.h"




/* ── Comandi HD44780 ─────────────────────────────── */
#define CMD_CLEARDISPLAY  0x01
#define CMD_RETURNHOME    0x02
#define CMD_ENTRYMODE     0x06   /* inc, no display shift */
#define CMD_DISPLAYON     0x0C   /* on, cursor off, blink off */
#define CMD_FUNCTIONSET   0x28   /* 4-bit, 2 lines, 5×8 */

/* Row 0 → DDRAM 0x00,  Row 1 → DDRAM 0x40 */
static const uint8_t ROW_OFFSET[] = {0x00, 0x40};





/* ── Basso livello ───────────────────────────────── */
static inline void pinHi(uint16_t pin) {
    HAL_GPIO_WritePin(LCD_GPIO_PORT, pin, GPIO_PIN_SET);
}
static inline void pinLo(uint16_t pin) {
    HAL_GPIO_WritePin(LCD_GPIO_PORT, pin, GPIO_PIN_RESET);
}

/* Impulso EN: ≥ 450 ns — con HAL_Delay(1) siamo a ~1 ms, abbondante */
static void pulseEN(void) {
    pinHi(LCD_EN_PIN);
    HAL_Delay(1);
    pinLo(LCD_EN_PIN);
    HAL_Delay(1);
}

/* Scrive i 4 bit alti di 'nibble' su D4-D7 */
static void write4(uint8_t n) {
    /* Scrittura pin individuali su stesso port — no race condition */
    (n & 0x01) ? pinHi(LCD_D4_PIN) : pinLo(LCD_D4_PIN);
    (n & 0x02) ? pinHi(LCD_D5_PIN) : pinLo(LCD_D5_PIN);
    (n & 0x04) ? pinHi(LCD_D6_PIN) : pinLo(LCD_D6_PIN);
    (n & 0x08) ? pinHi(LCD_D7_PIN) : pinLo(LCD_D7_PIN);
    pulseEN();
}

/*
 * send() — invia un byte completo in due nibble.
 * isData = 0 → comando (RS=0)
 * isData = 1 → dato   (RS=1)
 */
static void send(uint8_t val, uint8_t isData) {
    isData ? pinHi(LCD_RS_PIN) : pinLo(LCD_RS_PIN);
    write4(val >> 4);    /* high nibble prima */
    write4(val & 0x0F);  /* low  nibble dopo  */
}

static void cmd(uint8_t c)  { send(c, 0); HAL_Delay(2); }
static void dat(uint8_t d)  { send(d, 1); HAL_Delay(1); }








/* ── API pubblica ─────────────────────────────────── */
void LCD_Init(void) {
    /* Clock GPIOD già abilitato in MX_GPIO_Init() da CubeMX */
    HAL_Delay(50);                /* attendi stabilizzazione VCC */

    pinLo(LCD_RS_PIN);
    pinLo(LCD_EN_PIN);

    /* Sequenza speciale HD44780 §8.1.3 (inizializzazione da 8-bit) */
    write4(0x03); HAL_Delay(5);
    write4(0x03); HAL_Delay(5);
    write4(0x03); HAL_Delay(1);

    /* Passa a 4-bit */
    write4(0x02);

    /* Configurazione */
    cmd(CMD_FUNCTIONSET);   /* 4-bit, 2 righe, font 5×8 */
    cmd(CMD_DISPLAYON);     /* accendi display, cursore invisibile */
    cmd(CMD_CLEARDISPLAY);  /* cancella (richiede > 1.52 ms) */
    HAL_Delay(3);
    cmd(CMD_ENTRYMODE);     /* incrementa cursore a destra */
}

void LCD_Clear(void) {
    cmd(CMD_CLEARDISPLAY);
    HAL_Delay(3);
}

void LCD_SetCursor(uint8_t col, uint8_t row) {
    cmd(0x80 | (col + ROW_OFFSET[row & 1]));
}

void LCD_PrintChar(char c) { dat((uint8_t)c); }

void LCD_Print(const char* s) {
    while (*s) dat((uint8_t)*s++);
}


