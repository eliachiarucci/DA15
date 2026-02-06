/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f0xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define CC1_V_Pin GPIO_PIN_0
#define CC1_V_GPIO_Port GPIOA
#define CC2_V_Pin GPIO_PIN_1
#define CC2_V_GPIO_Port GPIOA
#define DN_V_Pin GPIO_PIN_2
#define DN_V_GPIO_Port GPIOA
#define DP_V_Pin GPIO_PIN_3
#define DP_V_GPIO_Port GPIOA
#define VBUS_V_Pin GPIO_PIN_0
#define VBUS_V_GPIO_Port GPIOB
#define DAC_MUTE_Pin GPIO_PIN_2
#define DAC_MUTE_GPIO_Port GPIOB
#define AMP_EN_Pin GPIO_PIN_12
#define AMP_EN_GPIO_Port GPIOB
#define BACK_I_Pin GPIO_PIN_13
#define BACK_I_GPIO_Port GPIOB
#define BACK_I_EXTI_IRQn EXTI4_15_IRQn
#define TRIM_B_Pin GPIO_PIN_14
#define TRIM_B_GPIO_Port GPIOB
#define TRIM_B_EXTI_IRQn EXTI4_15_IRQn
#define TRIM_A_Pin GPIO_PIN_15
#define TRIM_A_GPIO_Port GPIOB
#define TRIM_A_EXTI_IRQn EXTI4_15_IRQn
#define ENCODER_PUSH_I_Pin GPIO_PIN_8
#define ENCODER_PUSH_I_GPIO_Port GPIOA
#define ENCODER_PUSH_I_EXTI_IRQn EXTI4_15_IRQn
#define CONFIRM_I_Pin GPIO_PIN_9
#define CONFIRM_I_GPIO_Port GPIOA
#define CONFIRM_I_EXTI_IRQn EXTI4_15_IRQn
#define USB_MUX_SELECT_Pin GPIO_PIN_10
#define USB_MUX_SELECT_GPIO_Port GPIOA

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
