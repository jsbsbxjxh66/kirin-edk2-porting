#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/KirinWdt.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

// Periodic feeder. The hardware watchdog is armed for the 24 s SP805 default
// timeout and is reloaded every PcdKirinWdtKickPeriod (100 ns units) until the
// OS takes over at ExitBootServices.
STATIC EFI_EVENT mKickEvent = NULL;
STATIC EFI_EVENT mExitEvent = NULL;

STATIC
VOID
EFIAPI
WdtKickNotify(
    IN EFI_EVENT  Event,
    IN VOID       *Context
    )
{
  KirinWdtKick();
}

STATIC
VOID
EFIAPI
WdtExitBootServicesNotify(
    IN EFI_EVENT  Event,
    IN VOID       *Context
    )
{
  // gBS is about to go away, so the periodic feeder stops here. Hand the
  // board a stopped watchdog: the OS re-arms it from its own driver (the
  // kernel's hisi_sp805_wdt, enabled by default on Kirin970).
  KirinWdtDisable();
}

// WDVALUE counts down from WDLOAD at the SP805 input clock rate, so sampling
// it across a known delay measures that clock directly. KIRIN_WDT_LOAD_VAL is
// only valid if that clock is the 32764 Hz clkin_ref the DTS feeds the wd0
// mux; this makes the real rate visible in the log instead of assumed, so it
// can be compared against the kernel's "load_val=0x5ffcf" dmesg line.
// Returns 0 if the counter did not move.
STATIC
UINT32
MeasureWdtClockHz(VOID)
{
  UINT32  Before;
  UINT32  After;
  UINT32  Elapsed;

  Before = MmioRead32(KIRIN_WDT_BASE + KIRIN_WDT_VALUE);
  MicroSecondDelay(10000); // 10 ms -> ~328 ticks at 32768 Hz
  After = MmioRead32(KIRIN_WDT_BASE + KIRIN_WDT_VALUE);

  if (Before <= After) {
    return 0; // counter frozen (or a LOAD reload between the samples)
  }

  Elapsed = Before - After;
  return Elapsed * 100; // ticks per 10 ms -> Hz
}

EFI_STATUS
EFIAPI
KirinWdtEntryPoint(
    IN EFI_HANDLE         ImageHandle,
    IN EFI_SYSTEM_TABLE   *SystemTable
    )
{
  EFI_STATUS  Status;
  UINT64      Period;
  UINT32      Load;
  UINT32      ClockHz;

  if (!PcdGetBool(PcdKirinWdtEnable)) {
    DEBUG((EFI_D_ERROR, "KirinWdt: disabled by PcdKirinWdtEnable\n"));
    return EFI_SUCCESS;
  }

  Period = PcdGet64(PcdKirinWdtKickPeriod);

  Status = gBS->CreateEvent(
      EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, WdtKickNotify, NULL,
      &mKickEvent);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "KirinWdt: CreateEvent failed - %r\n", Status));
    return Status;
  }

  Status = gBS->SetTimer(mKickEvent, TimerPeriodic, Period);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "KirinWdt: SetTimer failed - %r\n", Status));
    return Status;
  }

  gBS->CreateEvent(
      EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY,
      WdtExitBootServicesNotify, NULL, &mExitEvent);

  // Only arm the hardware once the feeder is already running.
  KirinWdtEnable();

  Load    = MmioRead32(KIRIN_WDT_BASE + KIRIN_WDT_LOAD);
  ClockHz = MeasureWdtClockHz();

  DEBUG((EFI_D_ERROR, "KirinWdt: armed, LOAD=0x%X clk=%u Hz kick=%lu ms\n",
         Load, ClockHz, Period / 10000));

  return EFI_SUCCESS;
}
