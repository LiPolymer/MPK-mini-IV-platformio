/* OPEN MPK4: Artery driver selection (the library's at32f402_405_conf_template.h, reduced to the modules used).
 * HEXT_VALUE: the board's crystal, 12 MHz as in the stock BSP (hardware map section 1). */
#pragma once
#define HEXT_VALUE           ((uint32_t)12000000)
#define HEXT_STARTUP_TIMEOUT ((uint16_t)0x3000)
#define HICK_VALUE           ((uint32_t)8000000)
#define LEXT_VALUE           ((uint32_t)32768)
/* the driver sources compile only for enabled modules: exactly the ones the Makefile links */
#define ADC_MODULE_ENABLED
#define CRM_MODULE_ENABLED
#define DMA_MODULE_ENABLED
#define GPIO_MODULE_ENABLED
#define MISC_MODULE_ENABLED
#define PWC_MODULE_ENABLED
#define SPI_MODULE_ENABLED
#define TMR_MODULE_ENABLED
#define USART_MODULE_ENABLED
#include "at32f402_405_adc.h"
#include "at32f402_405_crm.h"
#include "at32f402_405_dma.h"
#include "at32f402_405_flash.h"   /* register layout and keys only (wait states, storage.c); no flash driver is linked */
#include "at32f402_405_gpio.h"
#include "at32f402_405_misc.h"
#include "at32f402_405_pwc.h"
#include "at32f402_405_spi.h"
#include "at32f402_405_tmr.h"
#include "at32f402_405_usart.h"
#include "at32f402_405_wdt.h"     /* register layout and command values only (board.c watchdog) */
