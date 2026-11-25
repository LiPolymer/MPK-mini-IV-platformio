/* OPEN MPK4: clocks as stock (app 0801d8ea): 12 MHz HEXT x 72 / 4 = 216 MHz, AHB /1, APB2 /1, APB1 /2,
 * PLLU /18 = 48 MHz for USB, flash wait states as stock (PSR 0x156), LDO 1.3 V. 1 kHz SysTick for g_ms.
 * The bootloader jumps in already running from the PLL, so crm_reset() first goes back to HICK. */
#include "at32f402_405.h"
#include "board.h"

volatile uint32_t g_ms;
RAMFUNC void SysTick_Handler(void) { g_ms++; }

bool clock_init(void)
{
    crm_reset();
    FLASH->psr = 0x156;                               /* stock value: 6 wait states for 216 MHz */
    crm_periph_clock_enable(CRM_PWC_PERIPH_CLOCK, TRUE);
    pwc_ldo_output_voltage_set(PWC_LDO_OUTPUT_1V3);
    crm_clock_source_enable(CRM_CLOCK_SOURCE_HEXT, TRUE);
    bool hext = crm_hext_stable_wait() == SUCCESS;
    if (hext) {
        crm_pll_config(CRM_PLL_SOURCE_HEXT, 72, 1, CRM_PLL_FP_4);
        crm_pllu_div_set(CRM_PLL_FU_18);
        crm_clock_source_enable(CRM_CLOCK_SOURCE_PLL, TRUE);
        while (crm_flag_get(CRM_PLL_STABLE_FLAG) != SET) { }
        crm_ahb_div_set(CRM_AHB_DIV_1);
        crm_apb2_div_set(CRM_APB2_DIV_1);
        crm_apb1_div_set(CRM_APB1_DIV_2);
        crm_auto_step_mode_enable(TRUE);
        crm_sysclk_switch(CRM_SCLK_PLL);
        while (crm_sysclk_switch_status_get() != CRM_SCLK_PLL) { }
        crm_auto_step_mode_enable(FALSE);
        crm_pllu_output_set(TRUE);
        while (crm_flag_get(CRM_PLLU_STABLE_FLAG) != SET) { }
        crm_usb_clock_source_select(CRM_USB_CLOCK_SOURCE_PLLU);
    }
    /* without a crystal we stay on HICK 8 MHz: slow, no USB, but the screen still reports it */
    system_core_clock_update();
    reduce_power_consumption();                       /* OTGHS PHY unused: power it down (Artery FAQ0148) */
    SysTick_Config(system_core_clock / 1000);
    NVIC_SetPriority(SysTick_IRQn, 3);
    return hext;
}
