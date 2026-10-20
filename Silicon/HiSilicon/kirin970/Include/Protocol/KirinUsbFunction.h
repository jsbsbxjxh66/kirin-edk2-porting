/** @file

  Mode control for the Kirin970 DWC3 gadget driver.

  gUsbDeviceProtocolGuid (EmbeddedPkg UsbDevice.h) can only *start* a USB
  function - it has no way to tear one down or to hand the controller back to
  the host-role xHCI driver.  The boot menu needs both:

    Idle() - bring up the default, consumer-less gadget.  The platform does
             NOT boot into it: the port comes up in the host (OTG) role that
             KirinDwc3Dxe and XhciDxe leave it in, so a PC sees nothing until
             something asks for a gadget.  This is what the usb-otg menu entry
             calls.

    Stop() - drop the gadget entirely, restore GCTL.PRTCAPDIR to HOST and
             re-bind XhciDxe, so a USB mass-storage stick can be plugged into
             the phone (USB host mode).  This is the state the port boots
             in, so calling it before any gadget was started is a no-op.

  A function is selected by calling gUsbDeviceProtocol->Start() with the new
  descriptors; that path internally tears down whatever was running and forces
  the host to re-enumerate.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef __KIRIN_USB_FUNCTION_H__
#define __KIRIN_USB_FUNCTION_H__

#include <Uefi.h>

#define KIRIN_USB_FUNCTION_PROTOCOL_GUID                              \
  {                                                                   \
    0x9b4e7c10, 0x5a23, 0x4d8f,                                       \
    { 0x91, 0x6c, 0x2e, 0x7a, 0x35, 0xd1, 0x4b, 0x60 }                \
  }

typedef struct _KIRIN_USB_FUNCTION_PROTOCOL KIRIN_USB_FUNCTION_PROTOCOL;

/**
  Tear the running gadget down, restore GCTL.PRTCAPDIR to HOST and re-bind
  the xHCI host controller driver.  No-op if no gadget was ever started,
  which is the state the port boots in.

  @retval EFI_SUCCESS  The block is now owned by the host role.
**/
typedef
EFI_STATUS
(EFIAPI *KIRIN_USB_FUNCTION_STOP)(VOID);

/**
  Bring the default gadget up: device descriptors with no consumer attached.
  Switches the port out of the host (OTG) role it boots in.

  @retval EFI_SUCCESS  The gadget is attached.
**/
typedef
EFI_STATUS
(EFIAPI *KIRIN_USB_FUNCTION_IDLE)(VOID);

/**
  Tell the driver how many bytes the host is about to send on the bulk OUT
  endpoint, so the OUT transfer can be armed for exactly that many.

  A bulk OUT transfer only terminates on a full buffer, a short packet or a
  ZLP.  Commands are <= 64 bytes against a 512-byte maxpacket, so the short
  packet ends them; a flash payload does not - 4624384 bytes is exactly
  9032 * 512 - so an arm sized 64 KB that is still holding the last 36864
  bytes when the host stops would sit there forever with no completion
  event, and fastboot would hang waiting for the download's OKAY.  Arming
  min(65536, Remaining) makes the last transfer fill its buffer exactly.

  The driver decrements its own count as data arrives and goes back to the
  default 64 KB arm once it reaches zero, which is what the next command
  needs.  Pass 0 to clear the expectation.

  @param[in] Bytes  Exact byte count the host will send next, or 0.

  @retval EFI_SUCCESS  The expectation was recorded.
**/
typedef
EFI_STATUS
(EFIAPI *KIRIN_USB_FUNCTION_SET_RX_LENGTH)(IN UINTN Bytes);

struct _KIRIN_USB_FUNCTION_PROTOCOL {
  KIRIN_USB_FUNCTION_STOP         Stop;
  KIRIN_USB_FUNCTION_IDLE         Idle;
  KIRIN_USB_FUNCTION_SET_RX_LENGTH  SetRxLength;
};

extern EFI_GUID  gKirinUsbFunctionProtocolGuid;

#endif
