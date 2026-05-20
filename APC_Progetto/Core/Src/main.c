/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    main.c
  * @brief   Smart Lock — STM32F3Discovery
  *          AS608  : USART2  PA2=TX  PA3=RX   57600 baud + EXTI5 su PC5
  *          ESP32  : USART3  PB10=TX PB11=RX 115200 baud (ricezione IT)
  *          LCD    : 4-bit parallelo GPIOD  PD8=RS PD9=EN PD10-PD13=D4-D7
  *          Buzzer : PB8
  *          Serratura: PB9 — HIGH=aperto (2 s) LOW=chiuso
  *          LED    : LD7 verde (PE11) · LD3 rosso (PE9)
  ******************************************************************************
  * Copyright (c) 2026 STMicroelectronics. All rights reserved.
  * This software is licensed under terms found in the LICENSE file.
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */
#include "lcd.h"      /* driver LCD 4-bit parallelo GPIOD */
#include "as608.h"    /* driver lettore impronta AS608    */
#include <string.h>
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef enum {
    S_STANDBY,      /* attesa eventi: TCH (PC5) o pulsante (PA0) */
    S_IDENTIFY,     /* scansione impronta con AS608               */
    S_WAIT_ESP,     /* attesa risposta AUTH_RESULT da ESP32       */
    S_RESULT_OK,    /* accesso concesso — LED verde + buzz lungo  */
    S_RESULT_FAIL,  /* accesso negato   — LED rosso + 3 bip       */
    S_ENROLL,       /* registrazione guidata nuova impronta       */
} AppState;
/* USER CODE END PTD */

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c1;
SPI_HandleTypeDef hspi1;
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart3;
PCD_HandleTypeDef hpcd_USB_FS;

/* USER CODE BEGIN PV */

static AppState appState = S_STANDBY;

/* ── Flag interrupt ─────────────────────────────────────────────────────────
 * Scritti nelle ISR (HAL_GPIO_EXTI_Callback), letti nel loop principale.
 * Dichiarati volatile per impedire ottimizzazioni del compilatore.         */
static volatile uint8_t flagFinger = 0;   /* PC5 TCH — dito rilevato       */
static volatile uint8_t flagButton = 0;   /* PA0     — pulsante blu premuto */

/* ── UART3 ricezione asincrona (ESP32 → STM32) ─────────────────────────── */
#define RX3_LEN   64
static uint8_t          rx3Byte;               /* buffer singolo byte IT     */
static char             rx3Buf[RX3_LEN];       /* riga accumulata            */
static uint8_t          rx3Idx   = 0;          /* indice di scrittura        */
static volatile uint8_t rx3Ready = 0;          /* flag: riga completa pronta */
static uint32_t         espTimer = 0;          /* timestamp avvio timeout    */

/* ── Ultimo ID identificato ─────────────────────────────────────────────── */
static uint8_t lastFid = 0;

/* ── Macro LED e Buzzer ─────────────────────────────────────────────────── */
#define LED_GREEN   LD7_Pin      /* PE11 — LD7 verde  */
#define LED_RED     LD3_Pin      /* PE9  — LD3 rosso  */
#define LED_PORT    GPIOE
#define BUZ_PIN     GPIO_PIN_8   /* PB8               */
#define BUZ_PORT    GPIOB
#define LOCK_PIN    GPIO_PIN_9   /* PB9 — serratura   */
#define LOCK_PORT   GPIOB        /* HIGH=aperto LOW=chiuso */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_SPI1_Init(void);
static void MX_USB_PCD_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_USART3_UART_Init(void);

/* USER CODE BEGIN PFP */
static void LED_Set(uint16_t pin, uint8_t on);
static void LED_OffAll(void);
static void Buzz(uint32_t ms);
static void BuzzBoot(void);
static void BuzzOK(void);
static void BuzzFail(void);
static void BuzzConfirm(void);
static void ShowLCD(const char* l1, const char* l2);
static void SendESP(const char* msg);
static void RunEnroll(void);
/* USER CODE END PFP */

/* USER CODE BEGIN 0 */

/* ── Callback EXTI ─────────────────────────────────────────────────────────
 * Gestisce i rising-edge di PC5 (TCH) e PA0 (pulsante).
 * Setta solo il flag — nessuna logica applicativa nell'ISR.                */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
    if (GPIO_Pin == GPIO_PIN_5) flagFinger = 1;   /* TCH AS608     */
    if (GPIO_Pin == GPIO_PIN_0) flagButton = 1;   /* pulsante PA0  */
}

/* ── Callback UART3 (ESP32 → STM32) ────────────────────────────────────────
 * Riceve un byte alla volta e assembla la riga fino a '\n'.
 * rx3Ready = 1 segnala che rx3Buf contiene una riga completa.
 * La callback si riarma immediatamente dopo ogni byte.                     */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef* h) {
    if (h->Instance == USART3) {
        char c = (char)rx3Byte;
        if (c == '\n') {
            rx3Buf[rx3Idx] = '\0';
            if (rx3Idx > 0) rx3Ready = 1;
            rx3Idx = 0;
        } else if (c != '\r' && rx3Idx < RX3_LEN - 1) {
            rx3Buf[rx3Idx++] = c;
        }
        HAL_UART_Receive_IT(&huart3, &rx3Byte, 1);
    }
}

/* ── Primitivi LED e Buzzer ─────────────────────────────────────────────── */

static void LED_Set(uint16_t pin, uint8_t on) {
    HAL_GPIO_WritePin(LED_PORT, pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
static void LED_OffAll(void) {
    LED_Set(LED_GREEN, 0);
    LED_Set(LED_RED,   0);
}
static void Buzz(uint32_t ms) {
    HAL_GPIO_WritePin(BUZ_PORT, BUZ_PIN, GPIO_PIN_SET);
    HAL_Delay(ms);
    HAL_GPIO_WritePin(BUZ_PORT, BUZ_PIN, GPIO_PIN_RESET);
}

/* Pattern buzzer:
 *   BuzzBoot    — 1 bip medio (200 ms)     : sistema pronto
 *   BuzzOK      — 1 bip lungo (500 ms)     : accesso concesso
 *   BuzzFail    — 3 bip rapidi (100 ms)    : accesso negato / errore
 *   BuzzConfirm — 1 bip breve (80 ms)      : conferma acquisizione immagine */
static void BuzzBoot(void)    { Buzz(200); }
static void BuzzOK(void)      { Buzz(500); }
static void BuzzFail(void)    { Buzz(100); HAL_Delay(80); Buzz(100); HAL_Delay(80); Buzz(100); }
static void BuzzConfirm(void) { Buzz(80);  }

/* ── Serratura ──────────────────────────────────────────────────────────── */
static void Lock_Open(void)  { HAL_GPIO_WritePin(LOCK_PORT, LOCK_PIN, GPIO_PIN_SET);   }
static void Lock_Close(void) { HAL_GPIO_WritePin(LOCK_PORT, LOCK_PIN, GPIO_PIN_RESET); }

/* ── Helper LCD ─────────────────────────────────────────────────────────── */
static void ShowLCD(const char* l1, const char* l2) {
    LCD_Clear();
    LCD_SetCursor(0, 0); LCD_Print(l1);
    LCD_SetCursor(0, 1); LCD_Print(l2);
}

/* ── Helper UART3 (invio verso ESP32) ──────────────────────────────────── */
static void SendESP(const char* msg) {
    char buf[80];
    uint16_t n = (uint16_t)snprintf(buf, sizeof(buf), "%s\r\n", msg);
    HAL_UART_Transmit(&huart3, (uint8_t*)buf, n, 1000);
}

/* ────────────────────────────────────────────────────────────────────────────
 * RunEnroll — procedura guidata di registrazione impronta
 *
 * Cerca prima il primo slot libero tramite AS608_FindFreeSlot(), che usa
 * LoadChar per sondare la libreria senza affidarsi a un contatore RAM.
 * Parte da slot 1 per evitare ID 0 non gestito dall'ESP32.
 *
 * Flusso:
 *   FindFreeSlot → GetImage → GenChar(1) → [dito tolto]
 *   → GetImage → GenChar(2) → RegModel → StoreChar(slot)
 * ───────────────────────────────────────────────────────────────────────── */
static void RunEnroll(void) {

    /* ── Cerca il primo slot libero nella libreria AS608 ── */
    ShowLCD("Ricerca slot.. ", "               ");
    int8_t freeSlot = AS608_FindFreeSlot();

    if (freeSlot < 0) {
        ShowLCD("DB Pieno!      ", "Max 127 impr.  ");
        BuzzFail();
        HAL_Delay(2000);
        return;
    }

    uint8_t  slot = (uint8_t)freeSlot;
    char     line[17];
    uint32_t t0;

    /* ── Passo 1: prima acquisizione ──────────────────────── */
    ShowLCD("Nuova Impronta ", "Appoggia dito  ");

    t0 = HAL_GetTick();
    while (!AS608_IsFingerPresent()) {
        if (HAL_GetTick() - t0 > 10000) goto enroll_timeout;
        HAL_Delay(100);
    }
    if (AS608_GetImage()               != AS608_OK) goto enroll_error;
    if (AS608_GenChar(AS608_CHARBUF_1) != AS608_OK) goto enroll_error;
    BuzzConfirm();

    /* ── Passo 2: rimozione dito ──────────────────────────── */
    ShowLCD("Togli il dito  ", "               ");
    while (AS608_IsFingerPresent()) HAL_Delay(100);
    HAL_Delay(500);   /* debounce meccanico */

    /* ── Passo 3: seconda acquisizione ───────────────────── */
    ShowLCD("Appoggia        ", "di nuovo       ");

    t0 = HAL_GetTick();
    while (!AS608_IsFingerPresent()) {
        if (HAL_GetTick() - t0 > 10000) goto enroll_timeout;
        HAL_Delay(100);
    }
    if (AS608_GetImage()               != AS608_OK) goto enroll_error;
    if (AS608_GenChar(AS608_CHARBUF_2) != AS608_OK) goto enroll_error;
    BuzzConfirm();

    /* ── Passo 4: genera modello e salva ─────────────────── */
    if (AS608_RegModel()      != AS608_OK) goto enroll_error;
    if (AS608_StoreChar(slot) != AS608_OK) goto enroll_error;

    /* ── Successo ─────────────────────────────────────────── */
    snprintf(line, sizeof(line), "Salvata ID: %u", (unsigned)slot);
    ShowLCD("Impronta OK!   ", line);
    BuzzOK();
    HAL_Delay(2000);
    return;

enroll_error:
    ShowLCD("Errore Registr.", "               ");
    BuzzFail();
    HAL_Delay(2000);
    return;

enroll_timeout:
    ShowLCD("Timeout        ", "Operaz. annull.");
    BuzzFail();
    HAL_Delay(2000);
}

/* USER CODE END 0 */

/* ============================================================================
 * main() — entry point
 * ========================================================================== */
int main(void)
{
  /* USER CODE BEGIN 1 */
  /* USER CODE END 1 */

  HAL_Init();

  /* USER CODE BEGIN Init */
  /* USER CODE END Init */

  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  /* USER CODE END SysInit */

  /* Inizializzazione periferiche generate da CubeMX */
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_SPI1_Init();
  MX_USB_PCD_Init();
  MX_USART2_UART_Init();
  MX_USART3_UART_Init();

  /* USER CODE BEGIN 2 */

  LCD_Init();
  AS608_Init(&huart2);

  /* ══════════════════════════════════════════════════════
   * SEQUENZA DI BOOT
   * ══════════════════════════════════════════════════════ */

  /* 1. Controllo reset hardware ─────────────────────────
   * Legge PA0 direttamente dopo MX_GPIO_Init().
   * Se il pulsante è tenuto premuto all'accensione, azzera
   * l'intera libreria biometrica dell'AS608.
   *
   * Strategia doppia:
   *   a) Prova EmptyDB (comando bulk rapido)
   *   b) Se LoadChar su slot 0 o 1 risponde ancora OK,
   *      EmptyDB non ha funzionato → cancella slot 0-127
   *      uno per uno con DeleteChar (fallback sicuro).    */
  if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_0) == GPIO_PIN_SET) {

      HAL_Delay(200);
      ShowLCD("Reset Database ", "In corso...    ");

      /* Handshake obbligatorio prima di qualsiasi comando */
      if (AS608_VerifyPassword() != AS608_OK) {
          ShowLCD("Errore Sensore ", "Reset annullato");
          BuzzFail();
          HAL_Delay(2000);
          goto boot_continue;
      }

      /* Tentativo 1: EmptyDB bulk */
      AS608_EmptyDB();
      HAL_Delay(1500);

      /* Verifica effettiva: slot 0 e slot 1 devono essere vuoti */
      uint8_t slot0_occupied = (AS608_LoadChar(AS608_CHARBUF_2, 0) == AS608_OK);
      uint8_t slot1_occupied = (AS608_LoadChar(AS608_CHARBUF_2, 1) == AS608_OK);

      if (slot0_occupied || slot1_occupied) {
          /* EmptyDB non ha funzionato — fallback: DeleteChar slot per slot */
          ShowLCD("Pulizia manuale", "Attendere...   ");
          uint8_t errCount = 0;
          for (uint8_t i = 0; i <= 127; i++) {
              /* Cancella solo se lo slot è occupato */
              if (AS608_LoadChar(AS608_CHARBUF_2, i) == AS608_OK) {
                  if (AS608_DeleteChar(i) != AS608_OK) errCount++;
              }
              HAL_Delay(50);
          }

          /* Verifica finale dopo cancellazione manuale */
          if (AS608_LoadChar(AS608_CHARBUF_2, 1) != AS608_OK) {
              ShowLCD("Database       ", "Azzerato!      ");
              Buzz(500);
          } else {
              ShowLCD("Errore Reset   ", "Riprovare      ");
              BuzzFail();
          }
      } else {
          /* EmptyDB ha funzionato */
          ShowLCD("Database       ", "Azzerato!      ");
          Buzz(500);
      }

      HAL_Delay(2000);
  }

boot_continue:

  /* 2. Splash screen ──────────────────────────────────── */
  ShowLCD("   Magic Box   ", " Progetto APC  ");
  BuzzBoot();
  HAL_Delay(1500);

  /* 3. Self-test sensore ────────────────────────────────
   * Handshake obbligatorio con l'AS608 tramite VFY_PWD.
   * In caso di fallimento il sistema si blocca: senza
   * sensore funzionante non ha senso proseguire.         */
  ShowLCD("Init sensore...", "               ");
  if (AS608_VerifyPassword() != AS608_OK) {
      ShowLCD("Errore Sensore ", "Sistema fermo  ");
      BuzzFail();
      while (1);   /* halt bloccante */
  }

  /* 4. Avvio ricezione asincrona ESP32 + notifica boot ── */
  HAL_UART_Receive_IT(&huart3, &rx3Byte, 1);
  SendESP("BOOT:SYSTEM_OK");

  appState = S_STANDBY;

  /* USER CODE END 2 */

  /* ══════════════════════════════════════════════════════
   * LOOP PRINCIPALE — macchina a stati
   * ══════════════════════════════════════════════════════ */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    switch (appState) {

    /* ── S_STANDBY ─────────────────────────────────────────
     * Stato di riposo. Mostra la schermata principale e rimane
     * in spin-wait finché uno dei due interrupt non arriva.
     * flagButton ha priorità: se entrambi scattano insieme
     * si avvia l'enrollment, non l'identificazione.          */
    case S_STANDBY:
        ShowLCD("   Magic Box   ", "Attesa impronta");
        LED_OffAll();
        flagFinger = 0;
        flagButton = 0;

        while (!flagFinger && !flagButton) HAL_Delay(50);

        if (flagButton) {
            flagButton = 0;
            appState   = S_ENROLL;
        } else {
            flagFinger = 0;
            appState   = S_IDENTIFY;
        }
        break;

    /* ── S_IDENTIFY ────────────────────────────────────────
     * Chiede all'AS608 di scansionare e cercare il dito nella
     * sua libreria flash (ricerca 1:N autonoma).
     * Se trovato, invia l'ID all'ESP32 per la verifica nel
     * database relazionale (nome, ruolo, permessi).
     *
     * CRITICO: rx3Ready e rx3Idx vengono azzerati PRIMA di
     * SendESP — se la risposta arriva durante ShowLCD (che
     * chiama HAL_Delay internamente), l'ISR setta rx3Ready=1
     * e il dato rimane valido. Azzerarlo dopo SendESP lo
     * cancellerebbe silenziosamente (race condition).        */
    case S_IDENTIFY: {
        ShowLCD("  Scansione... ", "               ");

        uint8_t  fid   = 0;
        uint16_t score = 0;
        AS608_Result r = AS608_IdentifyFinger(&fid, &score);

        if (r == AS608_OK) {
            lastFid = fid;
            char req[32];
            snprintf(req, sizeof(req), "AUTH_REQUEST:%d", fid);

            /* Pulisci buffer PRIMA di inviare la richiesta */
            rx3Ready = 0;
            rx3Idx   = 0;

            SendESP(req);
            ShowLCD("Verifica...    ", "               ");
            espTimer = HAL_GetTick();
            appState = S_WAIT_ESP;
        } else if (r == AS608_ERR_TIMEOUT) {
            /* AS608 non risponde alla UART */
            SendESP("ERR:AS608_TIMEOUT");
            appState = S_RESULT_FAIL;
        } else {
            /* no-match (0x09), immagine sfocata (0x03), ecc. */
            SendESP("ALERT:UNKNOWN_FINGER");
            appState = S_RESULT_FAIL;
        }
        break;
    }

    /* ── S_WAIT_ESP ────────────────────────────────────────
     * Attende la risposta dell'ESP32 entro 5 secondi.
     * Messaggio atteso: "AUTH_RESULT:<id>:GRANTED|DENIED"
     *
     * IMPORTANTE: il timeout è else-if — se rx3Ready è già
     * true nel medesimo ciclo il timeout non sovrascrive
     * il risultato (race condition fix).                   */
    case S_WAIT_ESP:
        if (rx3Ready) {
            rx3Ready = 0;
            appState = strstr(rx3Buf, "GRANTED") ? S_RESULT_OK
                                                  : S_RESULT_FAIL;
        } else if (HAL_GetTick() - espTimer > 10000) {
            SendESP("ERR:ESP32_TIMEOUT");
            appState = S_RESULT_FAIL;
        }
        HAL_Delay(50);
        break;

    /* ── S_RESULT_OK ───────────────────────────────────────
     * Accesso concesso. LED verde fisso per 2 s + bip lungo.
     * PB9 → HIGH (serratura aperta) per 2 s, poi LOW (chiusa). */
    case S_RESULT_OK: {
        char line2[17];
        snprintf(line2, sizeof(line2), "ID: %u", (unsigned)lastFid);
        ShowLCD("Accesso OK!    ", line2);
        LED_Set(LED_GREEN, 1);
        BuzzOK();
        Lock_Open();            /* PB9 HIGH — serratura aperta  */
        HAL_Delay(2000);
        Lock_Close();           /* PB9 LOW  — serratura chiusa  */
        LED_OffAll();
        appState = S_STANDBY;
        break;
    }

    /* ── S_RESULT_FAIL ─────────────────────────────────────
     * Accesso negato. LED rosso fisso per 2 s + 3 bip rapidi. */
    case S_RESULT_FAIL:
        ShowLCD("Accesso Negato ", "               ");
        LED_Set(LED_RED, 1);
        BuzzFail();
        HAL_Delay(2000);
        LED_OffAll();
        appState = S_STANDBY;
        break;

    /* ── S_ENROLL ──────────────────────────────────────────
     * Registrazione guidata di una nuova impronta.
     * La logica è in RunEnroll() per mantenere il switch
     * leggibile. Dopo il ritorno torna sempre a S_STANDBY. */
    case S_ENROLL:
        RunEnroll();
        appState = S_STANDBY;
        break;

    } /* end switch */

    /* USER CODE END WHILE */
    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * (contenuto generato da STM32CubeMX — non modificare)
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL6;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK) Error_Handler();

  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USB|RCC_PERIPHCLK_USART2
                              |RCC_PERIPHCLK_USART3|RCC_PERIPHCLK_I2C1;
  PeriphClkInit.Usart2ClockSelection = RCC_USART2CLKSOURCE_PCLK1;
  PeriphClkInit.Usart3ClockSelection = RCC_USART3CLKSOURCE_PCLK1;
  PeriphClkInit.I2c1ClockSelection = RCC_I2C1CLKSOURCE_HSI;
  PeriphClkInit.USBClockSelection = RCC_USBCLKSOURCE_PLL;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK) Error_Handler();
}

/* Funzioni di inizializzazione periferiche (generate da CubeMX) ------------ */

static void MX_I2C1_Init(void)
{
  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = 0x00201D2B;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK) Error_Handler();
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK) Error_Handler();
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK) Error_Handler();
}

static void MX_SPI1_Init(void)
{
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_4BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 7;
  hspi1.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
  hspi1.Init.NSSPMode = SPI_NSS_PULSE_ENABLE;
  if (HAL_SPI_Init(&hspi1) != HAL_OK) Error_Handler();
}

static void MX_USART2_UART_Init(void)
{
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 57600;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart2) != HAL_OK) Error_Handler();
}

static void MX_USART3_UART_Init(void)
{
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart3) != HAL_OK) Error_Handler();
}

static void MX_USB_PCD_Init(void)
{
  hpcd_USB_FS.Instance = USB;
  hpcd_USB_FS.Init.dev_endpoints = 8;
  hpcd_USB_FS.Init.speed = PCD_SPEED_FULL;
  hpcd_USB_FS.Init.phy_itface = PCD_PHY_EMBEDDED;
  hpcd_USB_FS.Init.low_power_enable = DISABLE;
  hpcd_USB_FS.Init.battery_charging_enable = DISABLE;
  if (HAL_PCD_Init(&hpcd_USB_FS) != HAL_OK) Error_Handler();
}

static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  HAL_GPIO_WritePin(GPIOE, CS_I2C_SPI_Pin|LD4_Pin|LD3_Pin|LD5_Pin
                          |LD7_Pin|LD9_Pin|LD10_Pin|LD8_Pin|LD6_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOD, LCD_RS_Pin|LCD_EN_Pin|LCD_D4_Pin|LCD_D5_Pin
                          |LCD_D6_Pin|LCD_D7_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8 | GPIO_PIN_9, GPIO_PIN_RESET);

  GPIO_InitStruct.Pin = DRDY_Pin|MEMS_INT3_Pin|MEMS_INT2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_EVT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = CS_I2C_SPI_Pin|LD4_Pin|LD3_Pin|LD5_Pin
                          |LD7_Pin|LD9_Pin|LD10_Pin|LD8_Pin|LD6_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = GPIO_PIN_5;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = LCD_RS_Pin|LCD_EN_Pin|LCD_D4_Pin|LCD_D5_Pin
                          |LCD_D6_Pin|LCD_D7_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = GPIO_PIN_8 | GPIO_PIN_9;  /* Buzzer PB8 · Serratura PB9 */
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);
  HAL_NVIC_SetPriority(EXTI9_5_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
}

/* USER CODE BEGIN 4 */
/* USER CODE END 4 */

void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  while (1) {}
  /* USER CODE END Error_Handler_Debug */
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* USER CODE END 6 */
}
#endif
