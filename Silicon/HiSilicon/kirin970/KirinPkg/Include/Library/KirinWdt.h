#ifndef _KIRIN_WDT_H_
#define _KIRIN_WDT_H_

// Kirin970 AP watchdog0 (ARM SP805). Register map and key/sequence taken
// from drivers/watchdog/hisi_sp805_wdt.c and
// drivers/hisi/ap/platform/kirin970/soc_rtctimerwdt_interface.h in the
// kirin970-cor-al00 android kernel tree; base address and timeout come from
// the "watchdog0" dt node (reg 0xe8a06000, default-timeout <0x18> = 24 s).
#define KIRIN_WDT_BASE            0xE8A06000
#define KIRIN_WDT_LOAD            0x000
#define KIRIN_WDT_VALUE           0x004
#define KIRIN_WDT_CONTROL         0x008
#define KIRIN_WDT_INTCLR          0x00C
#define KIRIN_WDT_RIS             0x010
#define KIRIN_WDT_LOAD_B          0x018
#define KIRIN_WDT_LOCK            0xC00

#define KIRIN_WDT_UNLOCK_KEY      0x1ACCE551
#define KIRIN_WDT_LOCK_KEY        0x00000001

// WDCONTROL_inten / WDCONTROL_resen
#define KIRIN_WDT_INT_ENABLE      0x1
#define KIRIN_WDT_RESET_ENABLE    0x2
// WDRIS_wdogris: value written to WDINTCLR to clear the interrupt
#define KIRIN_WDT_INTCLR_VALUE    0x1
// Counting clock, taken from the DTS exactly the way the kernel driver does.
// clk_mux_wd0 (kirin970_clock.dtsi) has parent[0] = clkin_ref = 32764 Hz and
// parent[1] = pclk_gate_wd0_high (cfgbus, tens of MHz), selected by
// CRGPERIPH_PERTIMECTRL.wdog0enov (reg 0x140, bit 0x20000); the select reads 0,
// so the SP805 ticks at clkin_ref. hisi_sp805_wdt.c therefore gets
// rate = 32764 Hz from clk_get_rate() and the default-timeout = 24 s from the
// watchdog0 dt node, and computes load in wdt_setload() as
//   load = (div_u64(rate, 2) * timeout) - 1
#define KIRIN_WDT_CLK_HZ          32764
#define KIRIN_WDT_TIMEOUT_S       24
// (32764 / 2) * 24 - 1 = 0x5FFCF, identical to the kernel's wdt->load_val
#define KIRIN_WDT_LOAD_VAL        ((KIRIN_WDT_CLK_HZ / 2) * KIRIN_WDT_TIMEOUT_S - 1)
// WDT_TIMEOUT_KICK (32768*3), a hardcoded literal in hisi_sp805_wdt.c, written
// to WDLOAD_B on enable
#define KIRIN_WDT_LOAD_B_VAL      (32768 * 3)

// Reload the counter with whatever load value is programmed (the kernel's
// wdt_ping() rewrites wdt->load_val), so it cannot expire while we are
// reprogramming or while we sit idle in the menu or shell.
VOID
EFIAPI
KirinWdtKick(VOID);

// Arm the watchdog with the 24 s default timeout (interrupt + reset).
VOID
EFIAPI
KirinWdtEnable(VOID);

// Stop the watchdog so it cannot reset the board while nobody feeds it.
VOID
EFIAPI
KirinWdtDisable(VOID);

#endif /* _KIRIN_WDT_H_ */
