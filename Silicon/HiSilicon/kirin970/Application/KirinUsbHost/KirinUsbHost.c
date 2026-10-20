/** @file

  Boot menu entry: switch the Kirin970 DWC3 port from peripheral (OTG) mode
  into host mode, so a USB device can be plugged into the phone.

  Stops the gadget, restores GCTL.PRTCAPDIR to HOST and re-binds XhciDxe.
  Host mode stays in effect after this application returns to the boot menu;
  it is undone by any of the peripheral-mode entries (fastboot, mass storage,
  adb, OTG) or on the next boot.

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

  Print (L"Switching the port to USB host mode ...\r\n");

  Status = Function->Stop ();
  if (EFI_ERROR (Status)) {
    Print (L"Could not enter host mode: %r\r\n", Status);
    return Status;
  }

  Print (L"\r\n");
  Print (L"USB host mode is ON.\r\n");
  Print (L"  Plug a USB stick, keyboard or hub into the phone now.\r\n");
  Print (L"  Host mode stays active after you leave this screen.\r\n");
  Print (L"\r\n");
  Print (L"Pick any USB-mode entry to go back to device (OTG) mode.\r\n");
  Print (L"\r\nPress the POWER key to go back to the boot menu.\r\n");

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
