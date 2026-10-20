/** @file

  Boot menu entry: restore the default peripheral (OTG) gadget after USB host
  mode was selected.  The port comes back up with the platform's idle
  descriptors and no function bound, which is the state the platform boots
  into by default.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Protocol/KirinUsbFunction.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  KIRIN_USB_FUNCTION_PROTOCOL  *Function;
  EFI_STATUS                    Status;
  EFI_INPUT_KEY                 Key;

  gBS->SetWatchdogTimer (0, 0x10000, 0, NULL);

  Status = gBS->LocateProtocol (
                  &gKirinUsbFunctionProtocolGuid,
                  NULL,
                  (VOID **)&Function
                  );
  if (EFI_ERROR (Status)) {
    Print (L"USB mode control unavailable: %r\r\n", Status);
    return Status;
  }

  Status = Function->Idle ();
  if (EFI_ERROR (Status)) {
    Print (L"Could not restore OTG mode: %r\r\n", Status);
    return Status;
  }

  Print (L"\r\n");
  Print (L"USB is back in OTG (device) mode, waiting on the host.\r\n");
  Print ( L"Press the POWER key to go back to the boot menu.\r\n");

  if ((gST->ConIn == NULL) || (gST->ConIn->WaitForKey == NULL)) {
    return EFI_SUCCESS;
  }

  ZeroMem (&Key, sizeof (Key));
  while (TRUE) {
    Status = gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, NULL);
    if (EFI_ERROR (Status)) {
      break;
    }
    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (EFI_ERROR (Status)) {
      continue;
    }
    if ((Key.ScanCode == SCAN_NULL) &&
        ((Key.UnicodeChar == CHAR_CARRIAGE_RETURN) ||
         (Key.UnicodeChar == L' ')))
    {
      break;
    }
  }

  return EFI_SUCCESS;
}
