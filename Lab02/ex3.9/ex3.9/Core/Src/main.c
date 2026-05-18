/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body - Bai 3.9 DS1307 + SH1106 + RC522 + UART log
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "sh1106.h"
#include "fonts.h"
#include "tm_stm32f4_mfrc522.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef struct
{
  uint8_t sec;
  uint8_t min;
  uint8_t hour;
  uint8_t weekday;
  uint8_t day;
  uint8_t month;
  uint8_t year;
} Time;

typedef struct
{
  Time time;
  uint8_t cardID[5];
  uint8_t accepted;   // 1 = Welcome, 0 = Rejected
} AccessLog;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define DS1307_ADDR                  (0x68 << 1)
#define DS1307_REG_TIME              0x00

/* Đổi thành 1 nếu muốn set lại giờ ban đầu cho DS1307.
 * Sau khi set xong một lần, nên để lại 0.
 */
#define SET_DS1307_TIME_ON_START     0

#define MAX_AUTH_CARDS               10
#define MAX_LOGS                     100
#define UART_CMD_BUF_SIZE            64

#define LED3_PORT                    GPIOG
#define LED3_PIN                     GPIO_PIN_13
#define LED4_PORT                    GPIOG
#define LED4_PIN                     GPIO_PIN_14

#define RC522_CS_PORT                GPIOE
#define RC522_CS_PIN                 GPIO_PIN_4
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c3;

SPI_HandleTypeDef hspi4;

TIM_HandleTypeDef htim6;

UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */
/* Danh sách mã thẻ hợp lệ.
 * Có thể nhập/thay đổi bằng Hercules:
 *   SET AA BB CC DD EE  : xóa danh sách cũ, lưu 1 thẻ mới vào AuthorizedCards[0]
 *   ADD AA BB CC DD EE  : thêm 1 thẻ mới vào AuthorizedCards
 *   LIST                : xem danh sách thẻ hợp lệ
 */

static uint8_t uart_rx_byte;

static char uart_cmd_buf[UART_CMD_BUF_SIZE];
static char uart_cmd_line[UART_CMD_BUF_SIZE];

static volatile uint8_t uart_cmd_ready = 0;
static volatile uint8_t uart_cmd_index = 0;

static uint8_t AuthorizedCards[MAX_AUTH_CARDS][5] = {
		{}
};
static uint8_t AuthorizedCount = 1;

static AccessLog Logs[MAX_LOGS];
static uint8_t LogCount = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C3_Init(void);
static void MX_SPI4_Init(void);
static void MX_TIM6_Init(void);
static void MX_USART1_UART_Init(void);
/* USER CODE BEGIN PFP */
static uint8_t DecToBCD(uint8_t val);
static uint8_t BCDToDec(uint8_t val);
static HAL_StatusTypeDef SetTime(Time *time);
static HAL_StatusTypeDef GetTime(Time *time);

static void UART_Print(const char *s);
static void UART_PrintCardID(const char *prefix, uint8_t cardID[5]);
static void ProcessUartRx(void);
static void ProcessCommand(char *cmd);
static uint8_t ParseCardID(char *text, uint8_t out[5]);

static void OLED_ClearLines(void);
static void OLED_ShowIdle(void);
static void OLED_ShowAccess(uint8_t accepted, uint8_t cardID[5]);

static uint8_t IsAuthorized(uint8_t cardID[5]);
static void AddLog(Time *time, uint8_t cardID[5], uint8_t accepted);
static void PrintLogs(void);
static void PrintAuthorizedCards(void);
static void HandleCard(uint8_t cardID[5]);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static uint8_t DecToBCD(uint8_t val)
{
  return (uint8_t)(((val / 10) << 4) | (val % 10));
}

static uint8_t BCDToDec(uint8_t val)
{
  return (uint8_t)(((val >> 4) * 10) + (val & 0x0F));
}

static HAL_StatusTypeDef SetTime(Time *time)
{
  uint8_t data[7];

  data[0] = DecToBCD(time->sec);
  data[1] = DecToBCD(time->min);
  data[2] = DecToBCD(time->hour);
  data[3] = DecToBCD(time->weekday);
  data[4] = DecToBCD(time->day);
  data[5] = DecToBCD(time->month);
  data[6] = DecToBCD(time->year);

  return HAL_I2C_Mem_Write(&hi2c3,
                           DS1307_ADDR,
                           DS1307_REG_TIME,
                           I2C_MEMADD_SIZE_8BIT,
                           data,
                           7,
                           1000);
}

static HAL_StatusTypeDef GetTime(Time *time)
{
  uint8_t data[7];
  HAL_StatusTypeDef status;

  status = HAL_I2C_Mem_Read(&hi2c3,
                            DS1307_ADDR,
                            DS1307_REG_TIME,
                            I2C_MEMADD_SIZE_8BIT,
                            data,
                            7,
                            1000);

  if (status != HAL_OK)
  {
    return status;
  }

  time->sec     = BCDToDec(data[0] & 0x7F);
  time->min     = BCDToDec(data[1]);
  time->hour    = BCDToDec(data[2] & 0x3F);
  time->weekday = BCDToDec(data[3]);
  time->day     = BCDToDec(data[4]);
  time->month   = BCDToDec(data[5]);
  time->year    = BCDToDec(data[6]);

  return HAL_OK;
}

static void UART_Print(const char *s)
{
  HAL_UART_Transmit(&huart1, (uint8_t *)s, strlen(s), 1000);
}

static void UART_PrintCardID(const char *prefix, uint8_t cardID[5])
{
  char buff[96];

  sprintf(buff, "%s%02X %02X %02X %02X %02X\r\n",
          prefix,
          cardID[0], cardID[1], cardID[2], cardID[3], cardID[4]);

  UART_Print(buff);
}

static void OLED_ClearLines(void)
{
  /* Xóa 3 dòng bằng cách ghi đè khoảng trắng, không phụ thuộc hàm clear của thư viện. */
  SH1106_GotoXY(0, 0);
  SH1106_Puts("           ", &Font_11x18, 1);

  SH1106_GotoXY(0, 22);
  SH1106_Puts("           ", &Font_11x18, 1);

  SH1106_GotoXY(0, 44);
  SH1106_Puts("           ", &Font_11x18, 1);
}

static void OLED_ShowIdle(void)
{
  OLED_ClearLines();

  SH1106_GotoXY(8, 10);
  SH1106_Puts("Scan Card", &Font_11x18, 1);

  SH1106_UpdateScreen();
}

static void OLED_ShowAccess(uint8_t accepted, uint8_t cardID[5])
{
  char line1[20];
  char line2[20];

  sprintf(line1, "%02X %02X %02X", cardID[0], cardID[1], cardID[2]);
  sprintf(line2, "%02X %02X", cardID[3], cardID[4]);

  OLED_ClearLines();

  SH1106_GotoXY(18, 0);
  if (accepted)
  {
    SH1106_Puts("Welcome", &Font_11x18, 1);
  }
  else
  {
    SH1106_Puts("Rejected", &Font_11x18, 1);
  }

  SH1106_GotoXY(0, 22);
  SH1106_Puts(line1, &Font_11x18, 1);

  SH1106_GotoXY(30, 44);
  SH1106_Puts(line2, &Font_11x18, 1);

  SH1106_UpdateScreen();
}

static uint8_t IsAuthorized(uint8_t cardID[5])
{
  uint8_t i;

  for (i = 0; i < AuthorizedCount; i++)
  {
    if (memcmp(cardID, AuthorizedCards[i], 5) == 0)
    {
      return 1;
    }
  }

  return 0;
}

static void AddLog(Time *time, uint8_t cardID[5], uint8_t accepted)
{
  uint8_t i;

  if (LogCount >= MAX_LOGS)
  {
    /* Nếu đầy 100 bản ghi, bỏ bản ghi cũ nhất. */
    for (i = 1; i < MAX_LOGS; i++)
    {
      Logs[i - 1] = Logs[i];
    }
    LogCount = MAX_LOGS - 1;
  }

  Logs[LogCount].time = *time;
  memcpy(Logs[LogCount].cardID, cardID, 5);
  Logs[LogCount].accepted = accepted;
  LogCount++;
}

static void PrintLogs(void)
{
  char buff[160];
  uint8_t i;

  UART_Print("\r\n===== ACCESS LOG =====\r\n");

  if (LogCount == 0)
  {
    UART_Print("No log\r\n");
    return;
  }

  for (i = 0; i < LogCount; i++)
  {
    sprintf(buff,
            "%03d. %02d:%02d:%02d %02d/%02d/20%02d | %02X %02X %02X %02X %02X | %s\r\n",
            i + 1,
            Logs[i].time.hour,
            Logs[i].time.min,
            Logs[i].time.sec,
            Logs[i].time.day,
            Logs[i].time.month,
            Logs[i].time.year,
            Logs[i].cardID[0],
            Logs[i].cardID[1],
            Logs[i].cardID[2],
            Logs[i].cardID[3],
            Logs[i].cardID[4],
            Logs[i].accepted ? "WELCOME" : "REJECTED");

    UART_Print(buff);
  }
}

static void PrintAuthorizedCards(void)
{
  char buff[96];
  uint8_t i;

  UART_Print("\r\n===== AUTHORIZED CARDS =====\r\n");

  if (AuthorizedCount == 0)
  {
    UART_Print("No authorized card. Use SET AA BB CC DD EE or ADD AA BB CC DD EE.\r\n");
    return;
  }

  for (i = 0; i < AuthorizedCount; i++)
  {
    sprintf(buff, "%02d. %02X %02X %02X %02X %02X\r\n",
            i + 1,
            AuthorizedCards[i][0],
            AuthorizedCards[i][1],
            AuthorizedCards[i][2],
            AuthorizedCards[i][3],
            AuthorizedCards[i][4]);
    UART_Print(buff);
  }
}

static uint8_t ParseCardID(char *text, uint8_t out[5])
{
  unsigned int b0, b1, b2, b3, b4;

  if (sscanf(text, "%x %x %x %x %x", &b0, &b1, &b2, &b3, &b4) == 5)
  {
    if (b0 <= 0xFF && b1 <= 0xFF && b2 <= 0xFF && b3 <= 0xFF && b4 <= 0xFF)
    {
      out[0] = (uint8_t)b0;
      out[1] = (uint8_t)b1;
      out[2] = (uint8_t)b2;
      out[3] = (uint8_t)b3;
      out[4] = (uint8_t)b4;
      return 1;
    }
  }

  return 0;
}

static void ProcessCommand(char *cmd)
{
  uint8_t card[5];
  uint8_t i;

  /* Chuyển lệnh về chữ hoa để dùng được cả a-f thường trong mã HEX. */
  for (i = 0; cmd[i] != '\0'; i++)
  {
    cmd[i] = (char)toupper((unsigned char)cmd[i]);
  }

  if (strcmp(cmd, "HELP") == 0)
  {
    UART_Print("\r\nCommands:\r\n");
    UART_Print("HELP                 : show commands\r\n");
    UART_Print("LOG                  : show access log\r\n");
    UART_Print("CLEARLOG             : clear log\r\n");
    UART_Print("LIST                 : show authorized cards\r\n");
    UART_Print("CLEARCARD            : clear authorized card list\r\n");
    UART_Print("SET AA BB CC DD EE   : replace authorized list by one card\r\n");
    UART_Print("ADD AA BB CC DD EE   : add one authorized card\r\n");
    return;
  }

  if (strcmp(cmd, "LOG") == 0)
  {
    PrintLogs();
    return;
  }

  if (strcmp(cmd, "CLEARLOG") == 0)
  {
    LogCount = 0;
    UART_Print("Log cleared\r\n");
    return;
  }

  if (strcmp(cmd, "LIST") == 0)
  {
    PrintAuthorizedCards();
    return;
  }

  if (strcmp(cmd, "CLEARCARD") == 0)
  {
    memset(AuthorizedCards, 0, sizeof(AuthorizedCards));
    AuthorizedCount = 0;
    UART_Print("Authorized card list cleared. Use SET or ADD to save card IDs.\r\n");
    return;
  }

  if (strncmp(cmd, "SET ", 4) == 0)
  {
    if (ParseCardID(cmd + 4, card))
    {
      memcpy(AuthorizedCards[0], card, 5);
      AuthorizedCount = 1;
      UART_PrintCardID("Set authorized card: ", card);
    }
    else
    {
      UART_Print("SET format error. Example: SET A1 B2 C3 D4 E5\r\n");
    }
    return;
  }

  if (strncmp(cmd, "ADD ", 4) == 0)
  {
    if (AuthorizedCount >= MAX_AUTH_CARDS)
    {
      UART_Print("Authorized card list is full\r\n");
      return;
    }

    if (ParseCardID(cmd + 4, card))
    {
      memcpy(AuthorizedCards[AuthorizedCount], card, 5);
      AuthorizedCount++;
      UART_PrintCardID("Added authorized card: ", card);
    }
    else
    {
      UART_Print("ADD format error. Example: ADD A1 B2 C3 D4 E5\r\n");
    }
    return;
  }

  UART_Print("Unknown command. Type HELP\r\n");
}

static void ProcessUartRx(void)
{
  char cmd[UART_CMD_BUF_SIZE];

  if (uart_cmd_ready)
  {
    __disable_irq();

    strcpy(cmd, uart_cmd_line);
    uart_cmd_ready = 0;

    __enable_irq();

    ProcessCommand(cmd);
  }
}

static void HandleCard(uint8_t cardID[5])
{
  Time now;
  uint8_t accepted;
  char buff[96];

  accepted = IsAuthorized(cardID);

  HAL_GPIO_WritePin(LED4_PORT, LED4_PIN, accepted ? GPIO_PIN_SET : GPIO_PIN_RESET);

  if (GetTime(&now) != HAL_OK)
  {
    now.sec = 0;
    now.min = 0;
    now.hour = 0;
    now.weekday = 0;
    now.day = 0;
    now.month = 0;
    now.year = 0;
    UART_Print("Read DS1307 ERROR, log time = 00:00:00\r\n");
  }

  AddLog(&now, cardID, accepted);
  OLED_ShowAccess(accepted, cardID);

  UART_PrintCardID("Card ID: ", cardID);
  sprintf(buff, "Result: %s | Time: %02d:%02d:%02d %02d/%02d/20%02d\r\n",
          accepted ? "WELCOME" : "REJECTED",
          now.hour, now.min, now.sec, now.day, now.month, now.year);
  UART_Print(buff);
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  uint8_t CardID[5];
  uint8_t LastCardID[5] = {0};
  uint8_t cardPresent = 0;
  uint8_t missCount = 0;
  Time setTime;
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C3_Init();
  MX_SPI4_Init();
  MX_TIM6_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */

  /* De-select RC522: SS/CS = HIGH */
  HAL_GPIO_WritePin(RC522_CS_PORT, RC522_CS_PIN, GPIO_PIN_SET);

  HAL_UART_Receive_IT(&huart1, &uart_rx_byte, 1);

  SH1106_Init();
  TM_MFRC522_Init();

#if SET_DS1307_TIME_ON_START
  setTime.sec = 10;
  setTime.min = 30;
  setTime.hour = 7;
  setTime.weekday = 4;
  setTime.day = 17;
  setTime.month = 4;
  setTime.year = 25;
  if (SetTime(&setTime) == HAL_OK)
  {
    UART_Print("Set DS1307 time OK\r\n");
  }
  else
  {
    UART_Print("Set DS1307 time ERROR\r\n");
  }
#else
  (void)setTime;
#endif

  OLED_ShowIdle();

  UART_Print("\r\nBai 3.9 ready\r\n");
  UART_Print("Type HELP for commands\r\n");
  PrintAuthorizedCards();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    ProcessUartRx();

    if (TM_MFRC522_Check(CardID) == MI_OK)
    {
      missCount = 0;
      HAL_GPIO_WritePin(LED3_PORT, LED3_PIN, GPIO_PIN_SET);

      if ((cardPresent == 0) || (memcmp(CardID, LastCardID, 5) != 0))
      {
        memcpy(LastCardID, CardID, 5);
        cardPresent = 1;
        HandleCard(CardID);
      }
    }
    else
    {
      if (cardPresent)
      {
        missCount++;
        if (missCount >= 3)
        {
          cardPresent = 0;
          missCount = 0;
          memset(LastCardID, 0, 5);

          HAL_GPIO_WritePin(LED3_PORT, LED3_PIN, GPIO_PIN_RESET);
          HAL_GPIO_WritePin(LED4_PORT, LED4_PIN, GPIO_PIN_RESET);

          UART_Print("Card removed\r\n");
          OLED_ShowIdle();
        }
      }
      else
      {
        HAL_GPIO_WritePin(LED3_PORT, LED3_PIN, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(LED4_PORT, LED4_PIN, GPIO_PIN_RESET);
      }
    }

    HAL_Delay(100);

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 180;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Activate the Over-Drive mode
  */
  if (HAL_PWREx_EnableOverDrive() != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief I2C3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C3_Init(void)
{

  /* USER CODE BEGIN I2C3_Init 0 */

  /* USER CODE END I2C3_Init 0 */

  /* USER CODE BEGIN I2C3_Init 1 */

  /* USER CODE END I2C3_Init 1 */
  hi2c3.Instance = I2C3;
  hi2c3.Init.ClockSpeed = 400000;
  hi2c3.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c3.Init.OwnAddress1 = 0;
  hi2c3.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c3.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c3.Init.OwnAddress2 = 0;
  hi2c3.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c3.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c3) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c3, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c3, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C3_Init 2 */

  /* USER CODE END I2C3_Init 2 */

}

/**
  * @brief SPI4 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI4_Init(void)
{

  /* USER CODE BEGIN SPI4_Init 0 */

  /* USER CODE END SPI4_Init 0 */

  /* USER CODE BEGIN SPI4_Init 1 */

  /* USER CODE END SPI4_Init 1 */
  /* SPI4 parameter configuration*/
  hspi4.Instance = SPI4;
  hspi4.Init.Mode = SPI_MODE_MASTER;
  hspi4.Init.Direction = SPI_DIRECTION_2LINES;
  hspi4.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi4.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi4.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi4.Init.NSS = SPI_NSS_SOFT;
  hspi4.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
  hspi4.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi4.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi4.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi4.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi4) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI4_Init 2 */

  /* USER CODE END SPI4_Init 2 */

}

/**
  * @brief TIM6 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM6_Init(void)
{

  /* USER CODE BEGIN TIM6_Init 0 */

  /* USER CODE END TIM6_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM6_Init 1 */

  /* USER CODE END TIM6_Init 1 */
  htim6.Instance = TIM6;
  htim6.Init.Prescaler = 0;
  htim6.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim6.Init.Period = 65535;
  htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim6) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim6, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM6_Init 2 */

  /* USER CODE END TIM6_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOE, GPIO_PIN_4, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOG, GPIO_PIN_13|GPIO_PIN_14, GPIO_PIN_RESET);

  /*Configure GPIO pin : PE4 */
  GPIO_InitStruct.Pin = GPIO_PIN_4;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /*Configure GPIO pins : PG13 PG14 */
  GPIO_InitStruct.Pin = GPIO_PIN_13|GPIO_PIN_14;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    uint8_t ch = uart_rx_byte;

    if (ch == '\r' || ch == '\n')
    {
      if (uart_cmd_index > 0 && uart_cmd_ready == 0)
      {
        uart_cmd_buf[uart_cmd_index] = '\0';
        strcpy(uart_cmd_line, uart_cmd_buf);

        uart_cmd_index = 0;
        uart_cmd_ready = 1;
      }
    }
    else
    {
      if (uart_cmd_ready == 0)
      {
        if (uart_cmd_index < UART_CMD_BUF_SIZE - 1)
        {
          uart_cmd_buf[uart_cmd_index++] = (char)ch;
        }
        else
        {
          uart_cmd_index = 0;
        }
      }
    }

    HAL_UART_Receive_IT(&huart1, &uart_rx_byte, 1);
  }
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
