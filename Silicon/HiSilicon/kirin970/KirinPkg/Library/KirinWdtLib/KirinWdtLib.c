#include <Base.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/KirinWdt.h>

// Flush posted writes (see hisi_sp805_wdt.c wdt_config()).
STATIC
VOID
WdtFlush(
    VOID
    )
{
  MmioRead32(KIRIN_WDT_BASE + KIRIN_WDT_LOCK);
}

VOID
EFIAPI
KirinWdtKick(
    VOID
    )
{
  UINT32 Load;

  MmioWrite32(KIRIN_WDT_BASE + KIRIN_WDT_LOCK, KIRIN_WDT_UNLOCK_KEY);

  Load = MmioRead32(KIRIN_WDT_BASE + KIRIN_WDT_LOAD);
  if (Load == 0)
    Load = KIRIN_WDT_LOAD_VAL;
  MmioWrite32(KIRIN_WDT_BASE + KIRIN_WDT_LOAD, Load);

  MmioWrite32(KIRIN_WDT_BASE + KIRIN_WDT_LOCK, KIRIN_WDT_LOCK_KEY);

  WdtFlush();
}

VOID
EFIAPI
KirinWdtEnable(
    VOID
    )
{
  // hisi_sp805_wdt.c wdt_config(!ping) -> wdt_enable()
  MmioWrite32(KIRIN_WDT_BASE + KIRIN_WDT_LOCK, KIRIN_WDT_UNLOCK_KEY);
  MmioWrite32(KIRIN_WDT_BASE + KIRIN_WDT_LOAD, KIRIN_WDT_LOAD_VAL);
  MmioWrite32(KIRIN_WDT_BASE + KIRIN_WDT_INTCLR, KIRIN_WDT_INTCLR_VALUE);
  MmioWrite32(
      KIRIN_WDT_BASE + KIRIN_WDT_CONTROL,
      KIRIN_WDT_INT_ENABLE | KIRIN_WDT_RESET_ENABLE);
  MmioWrite32(KIRIN_WDT_BASE + KIRIN_WDT_LOAD_B, KIRIN_WDT_LOAD_B_VAL);
  MmioWrite32(KIRIN_WDT_BASE + KIRIN_WDT_LOCK, KIRIN_WDT_LOCK_KEY);

  WdtFlush();
}

VOID
EFIAPI
KirinWdtDisable(
    VOID
    )
{
  // Kick first so it cannot expire while we are reprogramming it.
  KirinWdtKick();

  // hisi_sp805_wdt.c wdt_disable()
  MmioWrite32(KIRIN_WDT_BASE + KIRIN_WDT_LOCK, KIRIN_WDT_UNLOCK_KEY);
  MmioWrite32(KIRIN_WDT_BASE + KIRIN_WDT_CONTROL, 0);
  MmioWrite32(KIRIN_WDT_BASE + KIRIN_WDT_LOCK, KIRIN_WDT_LOCK_KEY);

  WdtFlush();
}
