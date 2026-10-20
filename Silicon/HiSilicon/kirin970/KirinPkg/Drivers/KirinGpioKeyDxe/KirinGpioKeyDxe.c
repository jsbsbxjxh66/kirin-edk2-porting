#include <Uefi.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PcdLib.h>
#include <Library/KirinPmicSpmiLib.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>
#include <Guid/ConsoleInDevice.h>

#define PL061_DATA_REG_OFFSET(pin)  (1U << ((pin) + 2))
#define PL061_DIR_REG  0x400

#define GPIO_BASE    FixedPcdGet64(PcdGpioBase)
#define VOL_UP_PIN   FixedPcdGet32(PcdVolumeUpGpio)
#define VOL_DOWN_PIN FixedPcdGet32(PcdVolumeDownGpio)

#define MAX_KEY_NOTIFIES  8

//
// The power key is not on the GPIO controller - it lives in the PMIC and has
// to be reached over SPMI. Device tree:
//   /spmi@FFF24000                 hisilicon,spmi-controller
//                                  spmi-channel = <0x02>
//   /spmi@FFF24000/pmic@0          slave_id = <0x00>
//     /ponkey@b1                   interrupts down = 7, up = 6
//   hisilicon,hisi-pmic-irq-addr   = <0x212 0x02>  (IRQ pending registers)
//
// The SPMI transaction itself comes from Library/KirinPmicSpmiLib.h; what is
// below is the ack part - drivers/mfd/hisi_pmic_spmi.c's hisi_irq_handler(),
// which is what reports the PON key in the running kernel.
//
#define PMIC_IRQ_PENDING      0x212
#define PMIC_IRQ_KEY_DOWN     7
#define PMIC_IRQ_KEY_UP       6
#define PMIC_IRQ_KEY_BITS     ((1U << PMIC_IRQ_KEY_DOWN) | (1U << PMIC_IRQ_KEY_UP))

typedef struct {
  UINTN                    Signature;
  EFI_KEY_DATA             KeyData;
  EFI_KEY_NOTIFY_FUNCTION  Function;
} KEY_NOTIFY_ENTRY;

STATIC EFI_EVENT  mTimerEvent;
STATIC EFI_EVENT  mWaitForKeyEvent;
STATIC EFI_EVENT  mWaitForKeyEventEx;

STATIC BOOLEAN    mVolUpPressed;
STATIC BOOLEAN    mVolDownPressed;
STATIC BOOLEAN    mPowerPressed;
STATIC BOOLEAN    mKeyPending;
STATIC UINT16     mPendingScanCode;
STATIC CHAR16     mPendingUnicode;

STATIC KEY_NOTIFY_ENTRY  mNotifyList[MAX_KEY_NOTIFIES];
STATIC UINTN             mNotifyCount;

STATIC
BOOLEAN
ReadGpioPin (
  IN UINTN  Pin
  )
{
  UINTN  Offset = PL061_DATA_REG_OFFSET (Pin);
  UINT8  Val    = MmioRead8 (GPIO_BASE + Offset);
  return (Val == 0);
}

//
// Returns TRUE on a power key press. The PMIC latches the down/up edges in
// the IRQ pending register until they are acknowledged, so polling cannot
// miss a short press; the bits are write-1-to-clear, and only the two bits we
// actually consumed are handed back, exactly like hisi_irq_handler() does.
//
STATIC
BOOLEAN
PollPowerKey (
  VOID
  )
{
  UINT8  Pending = 0;

  if (RETURN_ERROR (KirinPmicRead8 (PMIC_IRQ_PENDING, &Pending))) {
    return FALSE;
  }

  if ((Pending & PMIC_IRQ_KEY_BITS) == 0) {
    return FALSE;
  }

  KirinPmicWrite8 (PMIC_IRQ_PENDING, Pending & PMIC_IRQ_KEY_BITS);

  return (Pending & (1U << PMIC_IRQ_KEY_DOWN)) != 0;
}

STATIC
VOID
InvokeKeyNotifies (
  IN UINT16  ScanCode,
  IN CHAR16  Unicode
  )
{
  UINTN         Index;
  EFI_KEY_DATA  KeyData;

  ZeroMem (&KeyData, sizeof (KeyData));
  KeyData.Key.ScanCode    = ScanCode;
  KeyData.Key.UnicodeChar = Unicode;
  KeyData.KeyState.KeyShiftState = EFI_SHIFT_STATE_VALID;

  for (Index = 0; Index < mNotifyCount; Index++) {
    if (mNotifyList[Index].KeyData.Key.ScanCode == ScanCode &&
        mNotifyList[Index].KeyData.Key.UnicodeChar == Unicode) {
      mNotifyList[Index].Function (&KeyData);
    }
  }
}

STATIC
VOID
EFIAPI
KeyPollTimerHandler (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  BOOLEAN  UpNow    = ReadGpioPin (VOL_UP_PIN);
  BOOLEAN  DownNow  = ReadGpioPin (VOL_DOWN_PIN);
  BOOLEAN  PowerNow = PollPowerKey ();

  if (UpNow && !mVolUpPressed) {
    mKeyPending      = TRUE;
    mPendingScanCode = SCAN_UP;
    mPendingUnicode  = 0;
    InvokeKeyNotifies (SCAN_UP, 0);
  }

  if (DownNow && !mVolDownPressed) {
    mKeyPending      = TRUE;
    mPendingScanCode = SCAN_DOWN;
    mPendingUnicode  = 0;
    InvokeKeyNotifies (SCAN_DOWN, 0);
  }

  if (PowerNow && !mPowerPressed) {
    mKeyPending      = TRUE;
    mPendingScanCode = SCAN_NULL;
    mPendingUnicode  = CHAR_CARRIAGE_RETURN;
    InvokeKeyNotifies (SCAN_NULL, CHAR_CARRIAGE_RETURN);
  }

  mVolUpPressed   = UpNow;
  mVolDownPressed = DownNow;
  mPowerPressed   = PowerNow;

  if (mKeyPending) {
    gBS->SignalEvent (mWaitForKeyEvent);
    gBS->SignalEvent (mWaitForKeyEventEx);
  }
}

STATIC
EFI_STATUS
EFIAPI
KeyReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN BOOLEAN                          ExtendedVerification
  )
{
  mKeyPending      = FALSE;
  mPendingScanCode = SCAN_NULL;
  mPendingUnicode  = 0;
  mVolUpPressed    = FALSE;
  mVolDownPressed  = FALSE;
  mPowerPressed    = FALSE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KeyReadKeyStroke (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                   *Key
  )
{
  if (Key == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (!mKeyPending) {
    return EFI_NOT_READY;
  }

  Key->ScanCode    = mPendingScanCode;
  Key->UnicodeChar = mPendingUnicode;

  mKeyPending      = FALSE;
  mPendingScanCode = SCAN_NULL;
  mPendingUnicode  = 0;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KeyResetEx (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN BOOLEAN                             ExtendedVerification
  )
{
  mKeyPending      = FALSE;
  mPendingScanCode = SCAN_NULL;
  mPendingUnicode  = 0;
  mVolUpPressed    = FALSE;
  mVolDownPressed  = FALSE;
  mPowerPressed    = FALSE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KeyReadKeyStrokeEx (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  OUT EFI_KEY_DATA                       *KeyData
  )
{
  if (KeyData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (!mKeyPending) {
    return EFI_NOT_READY;
  }

  ZeroMem (KeyData, sizeof (EFI_KEY_DATA));
  KeyData->Key.ScanCode    = mPendingScanCode;
  KeyData->Key.UnicodeChar = mPendingUnicode;
  KeyData->KeyState.KeyShiftState = EFI_SHIFT_STATE_VALID;

  mKeyPending      = FALSE;
  mPendingScanCode = SCAN_NULL;
  mPendingUnicode  = 0;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KeySetState (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN EFI_KEY_TOGGLE_STATE              *KeyToggleState
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KeyRegisterKeyNotify (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN  EFI_KEY_DATA                       *KeyData,
  IN  EFI_KEY_NOTIFY_FUNCTION            KeyNotificationFunction,
  OUT VOID                               **NotifyHandle
  )
{
  if (KeyData == NULL || KeyNotificationFunction == NULL || NotifyHandle == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (mNotifyCount >= MAX_KEY_NOTIFIES) {
    return EFI_OUT_OF_RESOURCES;
  }

  mNotifyList[mNotifyCount].Signature = 0x4B455920;
  CopyMem (&mNotifyList[mNotifyCount].KeyData, KeyData, sizeof (EFI_KEY_DATA));
  mNotifyList[mNotifyCount].Function = KeyNotificationFunction;

  *NotifyHandle = (VOID *)&mNotifyList[mNotifyCount];
  mNotifyCount++;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KeyUnregisterKeyNotify (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN VOID                               *NotificationHandle
  )
{
  UINTN  Index;

  if (NotificationHandle == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Index < mNotifyCount; Index++) {
    if (&mNotifyList[Index] == NotificationHandle) {
      if (Index < mNotifyCount - 1) {
        CopyMem (&mNotifyList[Index], &mNotifyList[Index + 1],
          (mNotifyCount - Index - 1) * sizeof (KEY_NOTIFY_ENTRY));
      }
      mNotifyCount--;
      return EFI_SUCCESS;
    }
  }

  return EFI_INVALID_PARAMETER;
}

STATIC
VOID
EFIAPI
WaitForKeyCallback (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
}

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL mKeyProtocol = {
  KeyReset,
  KeyReadKeyStroke,
  NULL
};

STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL mKeyExProtocol = {
  KeyResetEx,
  KeyReadKeyStrokeEx,
  NULL,
  KeySetState,
  KeyRegisterKeyNotify,
  KeyUnregisterKeyNotify
};

STATIC EFI_HANDLE mKeyHandle = NULL;

EFI_STATUS
EFIAPI
KirinGpioKeyEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  MmioWrite8 (GPIO_BASE + PL061_DIR_REG,
    MmioRead8 (GPIO_BASE + PL061_DIR_REG) & ~((1 << VOL_UP_PIN) | (1 << VOL_DOWN_PIN)));

  mVolUpPressed    = FALSE;
  mVolDownPressed  = FALSE;
  mPowerPressed    = FALSE;
  mKeyPending      = FALSE;
  mPendingScanCode = SCAN_NULL;
  mPendingUnicode  = 0;
  mNotifyCount     = 0;

  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, WaitForKeyCallback, NULL, &mWaitForKeyEvent);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  mKeyProtocol.WaitForKey = mWaitForKeyEvent;

  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, WaitForKeyCallback, NULL, &mWaitForKeyEventEx);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  mKeyExProtocol.WaitForKeyEx = mWaitForKeyEventEx;

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, KeyPollTimerHandler, NULL, &mTimerEvent);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->SetTimer (mTimerEvent, TimerPeriodic, 500000);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mKeyHandle,
                  &gEfiSimpleTextInProtocolGuid,
                  &mKeyProtocol,
                  &gEfiSimpleTextInputExProtocolGuid,
                  &mKeyExProtocol,
                  &gEfiConsoleInDeviceGuid,
                  NULL,
                  NULL
                  );

  DEBUG ((DEBUG_INFO, "KirinGpioKey: VolUp=UP, VolDown=DOWN, Power=ENTER, GPIO base 0x%lx\n",
          (UINT64)GPIO_BASE));

  return Status;
}
