/* OPEN MPK4: startup for the AT32F405 running at 0x0800C000 behind the Akai bootloader.
 *
 * The bootloader jumps to *(0x0800C004) with interrupts masked, its own stack (MSP not reloaded) and VTOR still at
 * 0x08000000; after an update (C3) USB, SysTick, SPI2 and the GPIOs are still configured. So: load MSP, point VTOR
 * here, stop SysTick, clear the NVIC, reset every peripheral we use, then init RAM and enable interrupts.
 *
 * Every fault and every unexpected interrupt does NVIC_SystemReset(): a software reset without the power-on flag
 * makes the stock bootloader stay in update mode (update-protocol.md section 3), so a crash always lands somewhere
 * the flasher can reach. The reset flags are latched into g_reset_flags and cleared, so the next reset is judged on
 * its own flags: a watchdog reset (no POR, no SW flag) boots straight back into this firmware.
 *
 * VTOR points at a RAM copy of the vector table: while storage.c erases flash (13-16 ms, no flash reads possible) the
 * tick interrupt must still fetch its vector, and its handler lives in RAM too (board.h RAMFUNC). */
#include <stdint.h>
#include <string.h>
#include "at32f402_405.h"
#include "board.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
typedef void (*init_fn_t)(void);
extern init_fn_t __preinit_array_start[], __preinit_array_end[];
extern init_fn_t __init_array_start[], __init_array_end[];
int main(void);
void Reset_Handler(void);
void SysTick_Handler(void);
void PendSV_Handler(void);
void TMR3_GLOBAL_IRQHandler(void);
void OTGFS1_IRQHandler(void);

__attribute__((noreturn)) void Fault_Handler(void) { NVIC_SystemReset(); }
uint32_t g_reset_flags;

#define N_IRQ 104                                   /* IRQ 0..103 (ACC_IRQn is the last), 120 vectors in all */
typedef void (*vector_t)(void);
__attribute__((section(".isr_vector"), used))
const vector_t vectors[16 + N_IRQ] = {
    [0] = (vector_t)&_estack, [1] = Reset_Handler,
    [2 ... 13] = Fault_Handler,                       /* NMI, HardFault, MemManage, BusFault, UsageFault, SVC, ... */
    [14] = PendSV_Handler,                            /* board_service */
    [15] = SysTick_Handler,
    [16 ... 16 + TMR3_GLOBAL_IRQn - 1] = Fault_Handler,
    [16 + TMR3_GLOBAL_IRQn] = TMR3_GLOBAL_IRQHandler,
    [16 + TMR3_GLOBAL_IRQn + 1 ... 16 + OTGFS1_IRQn - 1] = Fault_Handler,
    [16 + OTGFS1_IRQn] = OTGFS1_IRQHandler,
    [16 + OTGFS1_IRQn + 1 ... 16 + N_IRQ - 1] = Fault_Handler,
};
/* VTOR alignment: the table size rounded up to a power of two (120 words -> 512 bytes) */
static vector_t ram_vectors[16 + N_IRQ] __attribute__((aligned(512)));
_Static_assert(sizeof ram_vectors <= 512, "ram_vectors alignment must cover the table");

__attribute__((naked, noreturn)) void Reset_Handler(void)
{
    __asm volatile ("cpsid i\n ldr r0, =_estack\n msr msp, r0\n b reset_c\n");
}

/* pulse the reset line of every peripheral the bootloader or this firmware drives */
static void periph_reset(void)
{
    const uint32_t ahb1 = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 5)   /* GPIOA-D, GPIOF */
                        | (1u << 22) | (1u << 24) | (1u << 29);                     /* DMA1, DMA2, OTGHS */
    const uint32_t ahb2 = 1u << 7;                                                  /* OTGFS1 */
    const uint32_t apb1 = (1u << 1) | (1u << 14);                                   /* TMR3, SPI2 */
    const uint32_t apb2 = (1u << 4) | (1u << 8) | (1u << 12);                       /* USART1, ADC, SPI1 */
    CRM->ahbrst1 |= ahb1; CRM->ahbrst2 |= ahb2; CRM->apb1rst |= apb1; CRM->apb2rst |= apb2;
    __DSB();
    CRM->ahbrst1 &= ~ahb1; CRM->ahbrst2 &= ~ahb2; CRM->apb1rst &= ~apb1; CRM->apb2rst &= ~apb2;
}

__attribute__((used, noreturn)) void reset_c(void)
{
    SCB->VTOR = (uint32_t)vectors;
    SysTick->CTRL = 0;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
    for (unsigned i = 0; i < sizeof NVIC->ICER / sizeof NVIC->ICER[0]; i++) { NVIC->ICER[i] = ~0u; NVIC->ICPR[i] = ~0u; }
    periph_reset();
    memcpy(&_sdata, &_sidata, (size_t)((char *)&_edata - (char *)&_sdata));
    memset(&_sbss, 0, (size_t)((char *)&_ebss - (char *)&_sbss));
    memcpy(ram_vectors, vectors, sizeof ram_vectors);
    SCB->VTOR = (uint32_t)ram_vectors;
    g_reset_flags = CRM->ctrlsts;
    CRM->ctrlsts_bit.rstfc = 1;
    NVIC_SetPriorityGrouping(3);                        /* 4 bits preemption, 0 sub (NVIC_PRIORITY_GROUP_4) */
    __DSB(); __ISB();
    for (init_fn_t *fn = __preinit_array_start; fn < __preinit_array_end; ++fn) (*fn)();
    for (init_fn_t *fn = __init_array_start; fn < __init_array_end; ++fn) (*fn)();
    __enable_irq();
    main();
    NVIC_SystemReset();
}
