#include "main.h"
/* USER CODE BEGIN Includes */
#include "carte.h"
/* USER CODE END Includes */

UART_HandleTypeDef huart2;            // PC (câble USB du ST-Link)

/* USER CODE BEGIN PV */
UART_HandleTypeDef huart4;            // vers le nœud suivant (aval)
UART_HandleTypeDef huart5;            // vers le nœud précédent (amont)
/* USER CODE END PV */

static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_UART4_Init(void);
static void MX_UART5_Init(void);

int main(void)
{
  HAL_Init();
  MX_GPIO_Init();
  MX_USART2_UART_Init();
  MX_UART4_Init();
  MX_UART5_Init();

  /* USER CODE BEGIN 2 */
  Carte_Init();
  /* USER CODE END 2 */

  while (1)
  {
    /* USER CODE BEGIN 3 */
    Carte_Process();
    /* USER CODE END 3 */
  }
}

// l'UART : 115200 bauds, 8 bits, pas de parité, 1 bit de stop
// (les broches PA2/PA3 et l'interruption sont réglées dans stm32f4xx_hal_msp.c)
static void MX_USART2_UART_Init(void)
{
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  HAL_UART_Init(&huart2);
}

// la LED (PA5) en sortie, le bouton USER B1 (PC13) en entrée
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  gpio.Pin = LD2_Pin;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  HAL_GPIO_Init(LD2_GPIO_Port, &gpio);

  gpio.Pin = B1_Pin;
  gpio.Mode = GPIO_MODE_INPUT;      // lu en boucle dans Carte_Process (avec anti-rebond)
  gpio.Pull = GPIO_NOPULL;          // résistance de tirage déjà présente sur la Nucleo
  HAL_GPIO_Init(B1_GPIO_Port, &gpio);
}

/* USER CODE BEGIN 4 */
// UART4 vers le nœud suivant : PC10 = TX (CN7-1), PC11 = RX (CN7-2), mêmes réglages que l'USART2
static void MX_UART4_Init(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_UART4_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  gpio.Pin = GPIO_PIN_10 | GPIO_PIN_11;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_PULLUP;          // RX au repos si rien n'est branché (dernier de la chaîne)
  gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  gpio.Alternate = GPIO_AF8_UART4;
  HAL_GPIO_Init(GPIOC, &gpio);

  huart4.Instance = UART4;
  huart4.Init.BaudRate = 115200;
  huart4.Init.WordLength = UART_WORDLENGTH_8B;
  huart4.Init.StopBits = UART_STOPBITS_1;
  huart4.Init.Parity = UART_PARITY_NONE;
  huart4.Init.Mode = UART_MODE_TX_RX;
  HAL_UART_Init(&huart4);

  // interruption UART4 : sans ça, HAL_UART_RxCpltCallback n'est jamais appelée
  HAL_NVIC_SetPriority(UART4_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(UART4_IRQn);
}

// UART5 vers le nœud précédent : PC12 = TX, PD2 = RX (connecteur CN7), mêmes réglages
static void MX_UART5_Init(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_UART5_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_PULLUP;          // RX au repos si rien n'est branché (master)
  gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  gpio.Alternate = GPIO_AF8_UART5;
  gpio.Pin = GPIO_PIN_12;
  HAL_GPIO_Init(GPIOC, &gpio);
  gpio.Pin = GPIO_PIN_2;
  HAL_GPIO_Init(GPIOD, &gpio);

  huart5.Instance = UART5;
  huart5.Init.BaudRate = 115200;
  huart5.Init.WordLength = UART_WORDLENGTH_8B;
  huart5.Init.StopBits = UART_STOPBITS_1;
  huart5.Init.Parity = UART_PARITY_NONE;
  huart5.Init.Mode = UART_MODE_TX_RX;
  HAL_UART_Init(&huart5);

  HAL_NVIC_SetPriority(UART5_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(UART5_IRQn);
}
/* USER CODE END 4 */

void Error_Handler(void)
{
  while (1) {}
}
