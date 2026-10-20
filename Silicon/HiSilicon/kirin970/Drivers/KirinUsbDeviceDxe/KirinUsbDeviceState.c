/** @file

  File-scope state for the DWC3 device-mode driver: the protocol template,
  descriptor storage, EP0/bulk bookkeeping, counters, the poll timer and the
  small static buffers returned to GET_DESCRIPTOR.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/
#include "KirinUsbDeviceInternal.h"

USB_DEVICE_PROTOCOL     mUsbDevice = {
  KirinUsbDeviceStart,
  KirinUsbDeviceSend
};

KIRIN_USB_FUNCTION_PROTOCOL mKirinUsbFunction = {
  KirinUsbDeviceStop,
  KirinUsbDeviceIdle,
  KirinUsbDeviceSetRxLength
};

//
// The port's resting state: one vendor-specific interface with the same
// bulk IN/OUT pair every other function uses, and no consumer attached, so
// whatever the host sends is dropped.  PID 0xff01 is deliberately outside
// Google's 0x4ee0-0x4ee7 block: 0x4ee7 is a real Google device ID that
// android_winusb.inf binds as "ADB Interface", so the idle gadget used
// to show up on the PC as an ADB device instead of the intended unknown one.
// Distinct from the fastboot pair (PcdAndroidFastbootUsbProductId 0xd00d,
// the Android Bootloader Interface ID, which is bound on purpose) so no host
// tool mistakes an idle port for a live fastboot target either.
//
#pragma pack(1)
typedef struct {
  USB_CONFIG_DESCRIPTOR       ConfigDescriptor;
  USB_INTERFACE_DESCRIPTOR    InterfaceDescriptor;
  USB_ENDPOINT_DESCRIPTOR     EndpointIn;
  USB_ENDPOINT_DESCRIPTOR     EndpointOut;
} IDLE_CONFIG_RESPONSE;
#pragma pack()

USB_DEVICE_DESCRIPTOR   mIdleDeviceDescriptor = {
  sizeof (USB_DEVICE_DESCRIPTOR),   // Length
  USB_DESC_TYPE_DEVICE,             // DescriptorType
  0x0200,                           // BcdUSB
  0xFF,                             // DeviceClass
  0,                                // DeviceSubClass
  0,                                // DeviceProtocol
  64,                               // MaxPacketSize0
  0x18d1,                           // IdVendor
  0xff01,                           // IdProduct (NOT a Google device ID)
  0,                                // BcdDevice
  0,                                // StrManufacturer
  0,                                // StrProduct
  0,                                // StrSerialNumber
  1                                 // NumConfigurations
};

STATIC IDLE_CONFIG_RESPONSE mIdleConfigResponse = {
  {
    sizeof (USB_CONFIG_DESCRIPTOR),               // Length
    USB_DESC_TYPE_CONFIG,                         // DescriptorType
    sizeof (IDLE_CONFIG_RESPONSE),                // TotalLength
    1,                                            // NumInterfaces
    1,                                            // ConfigurationValue
    0,                                            // Configuration
    BIT7 | BIT6,                                  // Attributes
    0                                             // MaxPower
  },
  {
    sizeof (USB_INTERFACE_DESCRIPTOR),            // Length
    USB_DESC_TYPE_INTERFACE,                      // DescriptorType
    0,                                            // InterfaceNumber
    0,                                            // AlternateSetting
    2,                                            // NumEndpoints
    0xFF,                                         // InterfaceClass
    0x42,                                         // InterfaceSubClass
    0x00,                                         // InterfaceProtocol
    0                                             // Interface
  },
  {
    sizeof (USB_ENDPOINT_DESCRIPTOR),             // Length
    USB_DESC_TYPE_ENDPOINT,                       // DescriptorType
    1 | BIT7,                                     // EndpointAddress
    0x2,                                          // Attributes
    512,                                          // MaxPacketSize
    16                                            // Interval
  },
  {
    sizeof (USB_ENDPOINT_DESCRIPTOR),             // Length
    USB_DESC_TYPE_ENDPOINT,                       // DescriptorType
    1,                                            // EndpointAddress
    0x2,                                          // Attributes
    512,                                          // MaxPacketSize
    16                                            // Interval
  }
};

VOID  *mIdleConfigDescriptors[] = {
  &mIdleConfigResponse,
  NULL
};

USB_DEVICE_RX_CALLBACK  mRxCallback;
USB_DEVICE_TX_CALLBACK  mTxCallback;
//
// The caller's descriptor is treated as read-only: it is copied into this
// before anything else, because the copy is what gets StrSerialNumber
// patched.  All three gadget callers ship StrSerialNumber = 0, and a host
// that asks for "string descriptor iSerialNumber" with index 0 gets the
// language list instead of a serial - which is exactly the single
// "setup 80 06 val 0300 idx 0409" line seen right before fastboot gives up
// on the device and never sends a bulk packet at all.
//
/* moved to KirinUsbDeviceInternal.h */
USB_DEVICE_DESCRIPTOR   *mDeviceDescriptor;
USB_DEVICE_DESCRIPTOR   mDeviceDescriptorCopy;

//
// The caller's descriptor array is only valid for the duration of Start()
// (see the snapshot in KirinUsbDeviceStart), so it is copied here.  Number of
// configurations actually copied, 0 means "nothing to serve".
//
/* moved to KirinUsbDeviceInternal.h */
UINTN                 mConfigDescriptorCount;
VOID                  *mConfigDescriptorArray[MAX_CONFIGURATIONS];
VOID                  **mConfigDescriptors;

BOOLEAN                 mInitialized;
BOOLEAN                 mDmaAllocated;
BOOLEAN                 mPollBusy;
UINT32                  mGadgetState;
BOOLEAN                 mConfigured;
//
// xHCI device handles taken away by DisconnectHostControllers(); kept so
// KirinUsbDeviceStop() can hand them back (LocateHandleBuffer would no
// longer find them - the Usb2Hc protocol is gone while we are a peripheral).
//
/* moved to KirinUsbDeviceInternal.h */
EFI_HANDLE              mHostHandles[MAX_HOST_HANDLES];
UINTN                   mHostHandleCount;
//
// Last step Dwc3CoreInit() reached.  Folded into the single failure line so
// the reason survives on a screen that can only show a handful of rows.
//
CONST CHAR8             *mInitStage = "init";

// DMA-visible allocations (page aligned)
UINT32                  *mEventBuffer;
UINT8                   *mCtrlRequest;
UINT8                   *mEp0Trb;
UINT8                   *mBulkInTrb;
UINT8                   *mBulkOutTrb;
UINT8                   *mTxDmaBuf;
UINT8                   *mRxDmaBuf;
UINT8                   *mEp0DataBuf;

UINTN                   mEventLpos;

//
// DSTS.USBLNKST as last reported by a link-status-change event, so the
// "wake up before STARTTRANSFER while parked in U1/U2/U3" rule has the same
// input the kernel's dwc->link_state does.
//
UINT8                   mLinkState;

// EP0 state
EP0_PHASE               mEp0Phase;
BOOLEAN                 mEp0ThreeStage;
BOOLEAN                 mEp0ExpectIn;
BOOLEAN                 mEp0NextStatus;
BOOLEAN                 mEp0Stalled;
BOOLEAN                 mEp0StallPending;
BOOLEAN                 mEp0Busy;
UINT32                  mEp0Resource;
//
// mEp0Resource == 0 does NOT mean "no transfer armed": on this core EP0-OUT
// is the first endpoint configured, so its transfer resource index is 0
// (OUT eps take 0..15, IN eps 16..31 - the same readback the kernel's
// dwc3_gadget_ep_get_transfer_index() returns).  Use this flag instead.
//
BOOLEAN                 mEp0ResourceValid;
UINT8                   mEp0ResourceEp;
USB_DEVICE_REQUEST      mSetup;
UINT8                   *mEp0Data;
UINTN                   mEp0DataLen;
UINT32                  mEp0MaxPacket = MAX_PACKET_EP0;
BOOLEAN                 mEp0Zero;

// Bulk state
BOOLEAN                 mBulkInBusy;
UINT32                  mBulkInResource;
UINTN                   mBulkInArmedLen;
UINT32                  mBulkInTaken;
BOOLEAN                 mBulkOutBusy;
UINT32                  mBulkOutResource;
UINTN                   mBulkOutArmedLen;
//
// TRB ring cursors, one pair per bulk endpoint.  They mirror dep->trb_enqueue
// / dep->trb_dequeue (core.h:609-610): the slot a new TRB is written to, and
// the slot whose write-back (residual) belongs to the completion we just
// observed.  Slot 255 is reserved for the Link TRB, so TrbInc() wraps 254 -> 0
// exactly like dwc3_ep_inc_trb() does.
//
UINT8                   mBulkInTrbEnq;
UINT8                   mBulkInTrbDeq;
UINT8                   mBulkOutTrbEnq;
UINT8                   mBulkOutTrbDeq;
//
// TX side: a silent bulk path is indistinguishable from a host that never
// opened a driver on a screen this small, so every submit and every completed
// submit is printed - fastboot only sends a handful of them per command.
//
// Progress counter: a 4.5 MB download is ~70 RX completions, so a line every
// 16 packets keeps ~5 lines on screen and shows exactly where a transfer
// that dies mid-way stops.
//
UINT32                  mBulkOutPackets;
UINT32                  mBulkOutBytes;
//
// Bytes the host promised after a "download:" command, still owed.  0 means
// "no expectation", i.e. arm the full RX_CHUNK_SIZE.  See
// KIRIN_USB_FUNCTION_SET_RX_LENGTH for why an exact arm is required at all.
//
UINTN                   mBulkOutRemaining;
//
// XFERINPROGRESS is the expected bulk completion, once per 64 KB chunk - 71
// of them for a 4.5 MB download.  Printing each one scrolls the tail of the
// screen away, so the line is emitted for #1 and then every 64th; anything
// that is not the expected event type still prints every time.
//
UINT32                  mBulkXferEventCount;
UINT32                  mDeviceEventCount;
UINT32                  mUnknownIrqCount;
//
// Why the most recent EpCmd() failed, folded into the "bulk IN arm failed"
// line so that a single transcribed line is self-contained instead of
// depending on the EpCmd line above it surviving the console scrollback.
//
CONST CHAR8             *mEpCmdFailReason;
UINT32                  mBulkNoDataCount;

EFI_EVENT               mPollTimer;


UINT8                   mStatusBuf[2];
UINT8                   mMiscBuf[1];
UINT8                   mStringZero[4];
UINT8                   mStringEmpty[2];
//
// Serial number served at KIRIN_SERIAL_STRING_INDEX.  No hardware UID is
// reachable here, so it is a fixed ASCII string widened to UTF-16LE; fastboot
// only needs *a* serial for `fastboot devices` to print one and for -s.
//
UINT8                   mSerialString[26] = {
  26, USB_DESC_TYPE_STRING,
  'K', 0, 'i', 0, 'r', 0, 'i', 0, 'n', 0, '9', 0,
  '7', 0, '0', 0, 'U', 0, 'E', 0, 'F', 0, 'I', 0
};
UINT8                   mQualifierBuf[10];
