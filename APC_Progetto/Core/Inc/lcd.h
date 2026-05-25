/*
 * lcd.h
 *
 *  Created on: May 15, 2026
 *      Author: dome-
 */

#ifndef INC_LCD_H_
#define INC_LCD_H_

#include "main.h"

/* Mappatura GPIO — GPIOD */
#define LCD_GPIO_PORT   GPIOD




#define LCD_RS_PIN      GPIO_PIN_8   /* PD8  */
/* RW collegato fisso a GND — nessun pin per RW     */
#define LCD_EN_PIN      GPIO_PIN_9   /* PD9  */
#define LCD_D4_PIN      GPIO_PIN_10  /* PD10 */
#define LCD_D5_PIN      GPIO_PIN_11  /* PD11 */
#define LCD_D6_PIN      GPIO_PIN_12  /* PD12 */
#define LCD_D7_PIN      GPIO_PIN_13  /* PD13 */


/* Funzioni */

void LCD_Init(void);
void LCD_Clear(void);
void LCD_SetCursor(uint8_t col, uint8_t row);
void LCD_Print(const char* str);
void LCD_PrintChar(char c);



#endif /* INC_LCD_H_ */
