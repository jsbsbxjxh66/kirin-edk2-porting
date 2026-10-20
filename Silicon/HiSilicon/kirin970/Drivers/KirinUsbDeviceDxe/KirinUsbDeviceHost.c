#include "KirinUsbDeviceInternal.h"

//
// ---------------------------------------------------------------------------
// Hand-over from host mode
// ---------------------------------------------------------------------------
//
// KirinDwc3Dxe publishes the controller as a NonDiscoverable xHCI device, so
// XhciDxe owns 0xFF100000 and keeps polling the root hub.  The port capability
// field in GCTL selects which of the two personalities the block decodes, so
// leaving xHCI bound while we switch to device mode means two drivers driving
// one controller: xHCI re-arms port events and fights our soft reset.
//
// Detaching it first is what hands the block over to us.  Host mode comes
// back on the next boot, which is the same trade-off the stock bootloader
// makes when it enters fastboot.
//
VOID
DisconnectHostControllers (
  VOID
  )
{
  EFI_HANDLE  *Handles;
  UINTN       Count;
  UINTN       Index;
  EFI_STATUS  Status;

  Count   = 0;
  Handles = NULL;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiUsb2HcProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status) || (Count == 0)) {
    return;
  }

  mHostHandleCount = 0;
  for (Index = 0; (Index < Count) && (mHostHandleCount < MAX_HOST_HANDLES); Index++) {
    mHostHandles[mHostHandleCount++] = Handles[Index];

    Status = gBS->DisconnectController (Handles[Index], NULL, NULL);
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: detached host controller %d: %r\n",
      Index,
      Status
      ));
  }

  FreePool (Handles);
}

//
// The exact inverse: restore the port capability KirinDwc3Dxe's
// Dwc3SetHostMode() set up, then let XhciDxe bind again.
//
VOID
SetPrtCapHost (
  VOID
  )
{
  UINT32  Reg;

  Reg = DwRead (DWC3_GCTL);
  Reg &= ~DWC3_GCTL_PRTCAP_MASK;
  Reg |= DWC3_GCTL_PRTCAPDIR (DWC3_GCTL_PRTCAP_HOST);
  Reg &= ~DWC3_GCTL_CORESOFTRESET;
  DwWrite (DWC3_GCTL, Reg);

  Reg = DwRead (DWC3_GUSB2PHYCFG0);
  Reg &= ~DWC3_GUSB2PHYCFG_SUSPHY;
  DwWrite (DWC3_GUSB2PHYCFG0, Reg);

  Reg = DwRead (DWC3_GUSB3PIPECTL0);
  Reg &= ~DWC3_GUSB3PIPECTL_SUSPHY;
  DwWrite (DWC3_GUSB3PIPECTL0, Reg);

  MicroSecondDelay (10000);
}

VOID
ConnectHostControllers (
  VOID
  )
{
  UINTN  Index;

  for (Index = 0; Index < mHostHandleCount; Index++) {
    gBS->ConnectController (mHostHandles[Index], NULL, NULL, TRUE);
  }
}
