/*
 * stm32f7xx_hal.h - host stand-in for the ST HAL and CMSIS headers
 *
 * Copyright (C) 2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 *
 * The host unit tests (tests/) compile firmware sources unchanged with the
 * computer's own C compiler. tests/shim comes first on the include path, so
 * this file replaces stm32f7xx_hal.h and stm32f7xx.h. It provides the types,
 * CMSIS intrinsics and peripheral handles that the application headers use,
 * implemented in portable C. Peripheral functions are only declared: a test
 * that links code calling them provides its own stub.
 *
 * Add to it as more firmware files come under test.
 */

#ifndef LXR_TEST_SHIM_STM32F7XX_HAL_H
#define LXR_TEST_SHIM_STM32F7XX_HAL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- CMSIS compiler macros ---------------------------------------------- */
#define __IO volatile
#define __I  volatile const
#define __O  volatile
#define __STATIC_INLINE static inline
#define __STATIC_FORCEINLINE static inline
#define __ASM __asm__
#define __NOP() ((void)0)
#define __DSB() ((void)0)
#define __ISB() ((void)0)
#define __DMB() ((void)0)
#define __enable_irq() ((void)0)
#define __disable_irq() ((void)0)

/* --- CMSIS / Cortex-M7 DSP intrinsics, portable versions ----------------- */
static inline uint8_t __CLZ(uint32_t x)
{
	return x ? (uint8_t)__builtin_clz(x) : 32;
}

static inline int32_t lxr_shim_ssat(int32_t x, unsigned bits)
{
	const int32_t max = (int32_t)((1u << (bits - 1)) - 1);
	const int32_t min = -max - 1;
	return x > max ? max : (x < min ? min : x);
}
#define __SSAT(x, bits) lxr_shim_ssat((int32_t)(x), (bits))

static inline uint32_t lxr_shim_usat(int32_t x, unsigned bits)
{
	const int32_t max = (int32_t)((1u << bits) - 1);
	return (uint32_t)(x > max ? max : (x < 0 ? 0 : x));
}
#define __USAT(x, bits) lxr_shim_usat((int32_t)(x), (bits))

/* Saturating add/subtract of two packed signed 16-bit halves. */
static inline uint32_t lxr_shim_q16(uint32_t a, uint32_t b, int sub)
{
	uint32_t r = 0;
	for (int h = 0; h < 2; h++) {
		const int32_t x = (int16_t)(a >> (16 * h));
		const int32_t y = (int16_t)(b >> (16 * h));
		const int32_t s = lxr_shim_ssat(sub ? x - y : x + y, 16);
		r |= ((uint32_t)s & 0xffffu) << (16 * h);
	}
	return r;
}
static inline uint32_t __QADD16(uint32_t a, uint32_t b) { return lxr_shim_q16(a, b, 0); }
static inline uint32_t __QSUB16(uint32_t a, uint32_t b) { return lxr_shim_q16(a, b, 1); }

/* --- Core peripherals ----------------------------------------------------- */
typedef struct
{
	__IO uint32_t CTRL;
	__IO uint32_t CYCCNT;
} DWT_Type;
extern DWT_Type lxr_shim_dwt;
#define DWT (&lxr_shim_dwt)

/* --- HAL types -------------------------------------------------------------- */
typedef enum
{
	HAL_OK      = 0x00U,
	HAL_ERROR   = 0x01U,
	HAL_BUSY    = 0x02U,
	HAL_TIMEOUT = 0x03U
} HAL_StatusTypeDef;

typedef enum
{
	GPIO_PIN_RESET = 0,
	GPIO_PIN_SET
} GPIO_PinState;

typedef enum
{
	RESET = 0U,
	SET = !RESET
} FlagStatus, ITStatus;

typedef enum
{
	DISABLE = 0U,
	ENABLE = !DISABLE
} FunctionalState;

/* Peripheral handles are opaque on the host; only pointers to them are used. */
typedef struct { int unused; } GPIO_TypeDef;
typedef struct { int unused; } I2S_HandleTypeDef;
typedef struct { int unused; } TIM_HandleTypeDef;
typedef struct { int unused; } UART_HandleTypeDef;
typedef struct { int unused; } ADC_HandleTypeDef;
typedef struct { int unused; } DMA_HandleTypeDef;
typedef struct { int unused; } SD_HandleTypeDef;
typedef struct { int unused; } RNG_HandleTypeDef;
typedef struct { int unused; } SPI_HandleTypeDef;
typedef struct { int unused; } PCD_HandleTypeDef;

typedef struct
{
	uint32_t CardType;
	uint32_t CardVersion;
	uint32_t Class;
	uint32_t RelCardAdd;
	uint32_t BlockNbr;
	uint32_t BlockSize;
	uint32_t LogBlockNbr;
	uint32_t LogBlockSize;
} HAL_SD_CardInfoTypeDef;

/* --- HAL functions (declared only, see above) ----------------------------- */
uint32_t HAL_GetTick(void);
uint32_t HAL_RCC_GetHCLKFreq(void);
void HAL_Delay(uint32_t delay);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin);

#ifdef __cplusplus
}
#endif

#endif /* LXR_TEST_SHIM_STM32F7XX_HAL_H */
