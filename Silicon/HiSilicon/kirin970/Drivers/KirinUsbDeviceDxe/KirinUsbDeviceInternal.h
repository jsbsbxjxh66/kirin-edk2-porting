/** @file

  Internal register map, shared state and cross-unit prototypes for the
  KirinUsbDeviceDxe translation units.

  The driver used to be a single 4048-line .c; it is now split the way the
  kernel splits dwc3/{core,gadget,ep0}.c - one unit per engine, with the
  bits they share declared here.

  SPDX-License-Identifier: BSD-2-Clause-Patent

*/

#ifndef KIRIN_USB_DEVICE_INTERNAL_H_
#define KIRIN_USB_DEVICE_INTERNAL_H_

/** @file

  USB device (peripheral) mode driver for the Synopsys DWC3 DRD controller
  found in the Kirin 970 (hisi_usb@ff200000 / dwc3@ff100000).

  Produces gUsbDeviceProtocolGuid, which is what the EmbeddedPkg
  AndroidFastbootTransportUsbDxe driver resolves at load time in order to
  run the stock AOSP fastboot 0.4 command engine over USB.

  Only USB 2.0 High Speed is supported (the device tree pins the port to
  maximum-speed = "high-speed" / phy_type = "utmi"), so SuperSpeed endpoint
  companion descriptors and the USB 3 link state machine are intentionally
  absent.

  Register programming follows the upstream DWC3 gadget model extracted from
  drivers/usb/dwc3/{core.c,gadget.c,ep0.c} of the Kirin970 kernel:

    - GCTL.PRTCAPDIR = device
    - DCTL soft reset, event buffer program, DEVTEN
    - DEPSTARTCFG(0) once, then DEPXFERCFG for every hardware endpoint
    - DEPSETEPCONFIG per endpoint, DALEPENA mask
    - DEPSTARTTRANSFER with a single TRB (LST|IOC|HWO) per transfer
    - GEVNTCOUNT polled from a 10 ms periodic timer (no GIC interrupt)

  A SETUP transfer is armed once with DEPTRBCTL_CONTROL_SETUP and re-armed
  after every status stage, exactly like dwc3_ep0_out_start() does.

  All DMA buffers are cache maintained explicitly: CPU written buffers are
  written back before DEPSTARTTRANSFER, controller written buffers are
  invalidated before the CPU reads them.
**/

#include <Uefi.h>
#include <Protocol/UsbDevice.h>
#include <Protocol/Usb2HostController.h>
#include <Protocol/KirinUsbFunction.h>
#include <IndustryStandard/Usb.h>

#include <Library/BaseLib.h>
#include <Library/KirinHi6523Lib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

//
// ---------------------------------------------------------------------------
// DWC3 register map (offsets are relative to dwc3@ff100000, i.e. the value
// returned by dwc3_readl() in io.h after it removes GLOBALS_REGS_START).
// ---------------------------------------------------------------------------
//
#define DWC3_BASE                 0xFF100000ULL

#define DWC3_GCTL                 0xC110
#define DWC3_GSNPSID              0xC120
#define DWC3_GHWPARAMS0           0xC140
#define DWC3_GHWPARAMS1           0xC144
#define DWC3_GHWPARAMS3           0xC14C
#define DWC3_GHWPARAMS7           0xC15C
#define DWC3_GUCTL2               0xC19C
#define DWC3_VER_NUMBER           0xC1A0
#define DWC3_GUSB2PHYCFG0         0xC200
#define DWC3_GUSB3PIPECTL0        0xC2C0
#define DWC3_GEVNTADRLO0          0xC400
#define DWC3_GEVNTADRHI0          0xC404
#define DWC3_GEVNTSIZ0            0xC408
#define DWC3_GEVNTCOUNT0          0xC40C

#define DWC3_DCFG                 0xC700
#define DWC3_DCTL                 0xC704
#define DWC3_DEVTEN               0xC708
#define DWC3_DSTS                 0xC70C
#define DWC3_DALEPENA             0xC720

#define DWC3_DEP_BASE(n)          (0xC800 + ((n) * 0x10))
#define DWC3_DEPCMDPAR2           0x00
#define DWC3_DEPCMDPAR1           0x04
#define DWC3_DEPCMDPAR0           0x08
#define DWC3_DEPCMD               0x0C

//
// GCTL
//
#define DWC3_GCTL_SCALEDOWN_MASK  (3u << 4)
#define DWC3_GCTL_PRTCAP_MASK     (3u << 12)
#define DWC3_GCTL_PRTCAPDIR(n)    ((n) << 12)
#define DWC3_GCTL_PRTCAP_DEVICE   2u
#define DWC3_GCTL_PRTCAP_HOST     1u
#define DWC3_GCTL_DSBLCLKGTNG     (1u << 0)
#define DWC3_GCTL_GBLHIBERNATIONEN (1u << 1)
#define DWC3_GCTL_CORESOFTRESET   (1u << 11)

// GUCTL2
//
// core.h:327 names this RST_ACTBITLATER, but its other meaning - the one
// dwc3_stop_active_transfer() documents at gadget.c:2686 - is "let software
// poll CMDACT to completion after an ENDTRANSFER".  Only present on DWC_usb3
// at revision >= 3.10a; on DWC_usb31 it does not exist (core.c:786-797), and
// without it an ENDTRANSFER issued to an already-finished transfer never
// clears CMDACT - which is what "ep3 cmd 0x8 timed out" was.
//
#define DWC3_GUCTL2_RST_ACTBITLATER (1u << 14)
#define DWC3_REVISION_310A       0x5533310a

//
// GHWPARAMS1 power optimisation (dwc3_core_init).
//
#define DWC3_GHWPARAMS1_EN_PWROPT(n) ((((n) >> 24) & 0x3u))
#define DWC3_GHWPARAMS1_EN_PWROPT_CLK 1u
#define DWC3_GHWPARAMS1_EN_PWROPT_HIB 2u

//
// Registers the kernel programs before the first endpoint command:
// dwc3_lscdtimer_set() (CONFIG_USB_DWC3_FEB values, dwc3-usb-interface.c)
// and the GRXTHRCFG tweak from __dwc3_gadget_start().
//
#define DWC3_GRXTHRCFG             0xC10C
#define DWC3_GRXTHRCFG_PKTCNTSEL   (1u << 29)
#define DWC3_GUCTL                 0xC12C
#define DWC3_GUCTL_ENOVERLAPCHK    (1u << 13)
#define DWC3_GUCTL1                0xC11C
#define DWC3_LU1LFPSTXTIM0         0xD004
#define DWC3_LSKIPFREQ0            0xD020
#define DWC3_LLUCTL0               0xD024
#define DWC3_LSCDTIM10             0xD02C
#define DWC3_LSCDTIM20             0xD030
#define DWC3_LLUCTL_PENDING_HP_TIMER_US_MASK (0x1Fu << 16)
#define DWC3_LLUCTL_PENDING_HP_TIMER_US(n)   (((n) & 0x1Fu) << 16)
#define DWC3_LLUCTL_ENABLE_HP_TIMER (1u << 16)

//
// DCTL / DSTS / DCFG
//
#define DWC3_DCTL_RUN_STOP        (1u << 31)
#define DWC3_DCTL_CSFTRST         (1u << 30)
//
// core.h:377-386 - DCTL test-mode field and the two "initiate" U1/U2 bits.
// dwc3_gadget_reset_interrupt() clears TSTCTRL so a host that left the device
// in TEST_J/TEST_K/TEST_PACKET stops driving the normal endpoints into a dead
// link, and dwc3_gadget_disconnect_interrupt() clears INITU1ENA/INITU2ENA so
// the next attach does not open a low-power state the host never offered.
//
#define DWC3_DCTL_TSTCTRL_MASK    (0xFu << 1)
#define DWC3_DCTL_ACCEPTU1ENA     (1u << 9)
#define DWC3_DCTL_INITU1ENA       (1u << 10)
#define DWC3_DCTL_ACCEPTU2ENA     (1u << 11)
#define DWC3_DCTL_INITU2ENA       (1u << 12)
//
// core.h:378 - DCTL.KEEP_CONNECT.  dwc3_gadget_run_stop() clears it on every
// connect for revisions >= 1.94a and sets it again only when the core reports
// hibernation support; leaving the bootloader's value in place keeps the D+
// pullup asserted across a soft reset, which is not the state the enumeration
// sequence expects.
//
#define DWC3_DCTL_KEEP_CONNECT    (1u << 19)
//
// core.h:389-398 / 415 / 647-661 - the link-state change request in DCTL and
// the link state itself in DSTS.  dwc3_send_gadget_ep_cmd() issues a remote
// wakeup before a STARTTRANSFER whenever the link is parked in U1/U2/U3.
//
#define DWC3_DCTL_ULSTCHNGREQ_MASK (0xFu << 5)
#define DWC3_DCTL_ULSTCHNGREQ(n)  (((n) << 5) & DWC3_DCTL_ULSTCHNGREQ_MASK)
#define DWC3_DSTS_DCNRD           (1u << 29)
#define DWC3_LINK_STATE_U0        0x00u
#define DWC3_LINK_STATE_U1        0x01u
#define DWC3_LINK_STATE_U2        0x02u
#define DWC3_LINK_STATE_U3        0x03u
#define DWC3_LINK_STATE_RX_DET    0x05u
#define DWC3_LINK_STATE_RECOV     0x08u
#define DWC3_LINK_STATE_MASK      0x0Fu
#define DWC3_DSTS_DEVCTRLHLT      (1u << 22)
#define DWC3_DSTS_USBLNKST(r)     (((r) >> 18) & 0x0Fu)
#define DWC3_DSTS_CONNECTSPD      7u
#define DWC3_DSTS_HIGHSPEED       0u
#define DWC3_DSTS_FULLSPEED       1u
#define DWC3_DSTS_LOWSPEED        2u
#define DWC3_DSTS_SUPERSPEED      4u
#define DWC3_DSTS_SUPERSPEED_PLUS 5u
#define DWC3_DCFG_DEVADDR(a)      (((a) & 0x7F) << 3)
#define DWC3_DCFG_DEVADDR_MASK    (0x7Fu << 3)
#define DWC3_DCFG_HIGHSPEED       (0u << 0)
#define DWC3_DCFG_FULLSPEED       (1u << 0)
#define DWC3_DCFG_LOWSPEED        (2u << 0)
#define DWC3_DCFG_NUMP_MASK       (0x1Fu << 17)
#define DWC3_DCFG_NUMP(n)         (((n) & 0x1Fu) << 17)
#define DWC3_DCFG_NUMP_GET(n)     (((n) >> 17) & 0x1Fu)

#define DWC3_GHWPARAMS0_MDWIDTH(n)      (((n) >> 8) & 0xFFu)
#define DWC3_GHWPARAMS7_RAM2_DEPTH(n)   (((n) >> 16) & 0xFFFFu)

//
// DEVTEN
//
#define DWC3_DEVTEN_VNDRDEVTSTRCVEDEN (1u << 12)
#define DWC3_DEVTEN_EVNTOVERFLOWEN    (1u << 11)
#define DWC3_DEVTEN_CMDCMPLTEN        (1u << 10)
#define DWC3_DEVTEN_ERRTICERREN       (1u << 9)
#define DWC3_DEVTEN_EOPFEN            (1u << 6)
#define DWC3_DEVTEN_WKUPEVTEN         (1u << 4)
#define DWC3_DEVTEN_ULSTCNGEN         (1u << 3)
#define DWC3_DEVTEN_CONNECTDONEEN     (1u << 2)
#define DWC3_DEVTEN_USBRSTEN          (1u << 1)
#define DWC3_DEVTEN_DISCONNEVTEN      (1u << 0)
//
// Exactly dwc3_gadget_enable_irq()'s mask (gadget.c:1873-1882): every one of
// these bits but DISCONNECT/USBRST/CONNECTDONE/ULSTCNG/ERRTICER was missing
// before, so the core had no way to tell us that the event buffer had
// overflowed (DWC3_DEVICE_EVENT_OVERFLOW) or that the link had gone to
// suspend (DWC3_DEVICE_EVENT_EOPF on a >=2.30a core) - the two conditions
// that look identical to "the host stopped reading" from the outside.
//

//
// GUSB2PHYCFG
//
#define DWC3_GUSB2PHYCFG_SUSPHY   (1u << 6)
#define DWC3_GUSB2PHYCFG_ULPI_UTMI (1u << 4)
#define DWC3_GUSB2PHYCFG_PHYIF(n)  ((n) << 3)
#define DWC3_GUSB2PHYCFG_PHYIF_MASK DWC3_GUSB2PHYCFG_PHYIF(1)
#define DWC3_GUSB2PHYCFG_USBTRDTIM(n) ((n) << 10)
#define DWC3_GUSB2PHYCFG_USBTRDTIM_MASK DWC3_GUSB2PHYCFG_USBTRDTIM(0xF)
#define USBTRDTIM_UTMI_8_BIT      9

//
// GUSB3PIPECTL
//
#define DWC3_GUSB3PIPECTL_SUSPHY  (1u << 17)
#define DWC3_GUSB3PIPECTL_UX_EXIT_PX (1u << 27)

//
// GEVNT*
//
#define DWC3_GEVNTCOUNT_MASK      0xFFFCu
#define DWC3_GEVNTSIZ_INTMASK     (1u << 31)
#define DWC3_GEVNTSIZ_SIZE(n)     ((n) & 0xFFFFu)

//
// DEPCMD
//
#define DWC3_DEPCMD_SETEPCONFIG      (0x01u << 0)
#define DWC3_DEPCMD_SETTRANSFRESOURCE (0x02u << 0)
#define DWC3_DEPCMD_SETSTALL         (0x04u << 0)
#define DWC3_DEPCMD_CLEARSTALL       (0x05u << 0)
#define DWC3_DEPCMD_STARTTRANSFER    (0x06u << 0)
#define DWC3_DEPCMD_UPDATETRANSFER   (0x07u << 0)
#define DWC3_DEPCMD_ENDTRANSFER      (0x08u << 0)
#define DWC3_DEPCMD_DEPSTARTCFG      (0x09u << 0)
#define DWC3_DEPCMD_CMDACT            (1u << 10)
#define DWC3_DEPCMD_HIPRI_FORCERM     (1u << 11)
#define DWC3_DEPCMD_CMDIOC            (1u << 8)
#define DWC3_DEPCMD_PARAM(x)          ((x) << 16)
#define DWC3_DEPCMD_CMD(x)            ((x) & 0x0Fu)
#define DWC3_DEPCMD_STATUS(x)         (((x) >> 12) & 0x0Fu)
#define DWC3_DEPCMD_GET_RSC_IDX(x)    (((x) >> 16) & 0x7Fu)
//
// DEPEVT_TRANSFER_NO_RESOURCE / DEPEVT_TRANSFER_BUS_EXPIRY (core.h:1125-1126).
// These two are the only status values dwc3_send_gadget_ep_cmd() turns into an
// error, for every command it can issue.
//
#define DWC3_DEPCMD_ST_NO_RSC         1u
#define DWC3_DEPCMD_ST_BUS_EXPIRY     2u

//
// DEPCFG (DEPCMDPAR0 / DEPCMDPAR1)
//
#define DWC3_DEPCFG_EP_TYPE(n)        (((n) & 0x3) << 1)
#define DWC3_DEPCFG_MAX_PACKET_SIZE(n) (((n) & 0x7FF) << 3)
#define DWC3_DEPCFG_FIFO_NUMBER(n)    (((n) & 0x1F) << 17)
#define DWC3_DEPCFG_ACTION_INIT       (0u << 30)
#define DWC3_DEPCFG_ACTION_MODIFY     (2u << 30)
#define DWC3_DEPCFG_XFER_COMPLETE_EN  (1u << 8)
#define DWC3_DEPCFG_XFER_IN_PROGRESS_EN (1u << 9)
#define DWC3_DEPCFG_XFER_NOT_READY_EN (1u << 10)
#define DWC3_DEPCFG_EP_NUMBER(n)      (((n) & 0x1F) << 25)
#define DWC3_DEPCFG_BINTERVAL_M1(n)   (((n) & 0xFF) << 16)
#define DWC3_DEPXFERCFG_NUM_XFER_RES(n) ((n) & 0xFFFFu)

//
// TRB
//
#define DWC3_TRB_SIZE_LENGTH(n)       ((n) & 0x00FFFFFFu)
#define DWC3_TRB_SIZE_TRBSTS(n)       (((n) & (0x0Fu << 28)) >> 28)
#define DWC3_TRBSTS_SETUP_PENDING     2
#define DWC3_TRB_CTRL_TRBCTL(n)       (((n) & 0x3F) << 4)
#define DWC3_TRB_CTRL_HWO             (1u << 0)
#define DWC3_TRB_CTRL_LST             (1u << 1)
#define DWC3_TRB_CTRL_CHN             (1u << 2)
#define DWC3_TRB_CTRL_CSP             (1u << 3)
#define DWC3_TRB_CTRL_ISP_IMI         (1u << 10)
#define DWC3_TRB_CTRL_IOC             (1u << 11)
#define DWC3_TRBCTL_NORMAL            DWC3_TRB_CTRL_TRBCTL(1)
#define DWC3_TRBCTL_CONTROL_SETUP     DWC3_TRB_CTRL_TRBCTL(2)
#define DWC3_TRBCTL_CONTROL_STATUS2   DWC3_TRB_CTRL_TRBCTL(3)
#define DWC3_TRBCTL_CONTROL_STATUS3   DWC3_TRB_CTRL_TRBCTL(4)
#define DWC3_TRBCTL_CONTROL_DATA      DWC3_TRB_CTRL_TRBCTL(5)
#define DWC3_TRBCTL_LINK_TRB          DWC3_TRB_CTRL_TRBCTL(8)

//
// Device / endpoint events
//
#define DWC3_EVENT_TYPE_DEV           0
#define DWC3_DEVICE_EVENT_DISCONNECT         0
#define DWC3_DEVICE_EVENT_RESET              1
#define DWC3_DEVICE_EVENT_CONNECT_DONE       2
#define DWC3_DEVICE_EVENT_LINK_STATUS_CHANGE 3
#define DWC3_DEVICE_EVENT_WAKEUP             4
#define DWC3_DEVICE_EVENT_HIBER_REQ          5
#define DWC3_DEVICE_EVENT_EOPF               6
#define DWC3_DEVICE_EVENT_SOF                7
#define DWC3_DEVICE_EVENT_ERRATIC_ERROR      9
#define DWC3_DEVICE_EVENT_CMD_CMPL           10
#define DWC3_DEVICE_EVENT_OVERFLOW           11

#define DWC3_DEPEVT_XFERCOMPLETE      0x01
#define DWC3_DEPEVT_XFERINPROGRESS    0x02
#define DWC3_DEPEVT_XFERNOTREADY      0x03
#define DWC3_DEPEVT_EPCMDCMPLT        0x07

#define DEPEVT_STATUS_CONTROL_DATA    1
#define DEPEVT_STATUS_CONTROL_STATUS  2

//
// ---------------------------------------------------------------------------
// Controller topology
// ---------------------------------------------------------------------------
//
#define DWC3_EVENT_BUFFER_SIZE   4096
#define DWC3_TRB_SIZE            16
//
// TRB ring size, per endpoint.  The kernel allocates DWC3_TRB_NUM (core.h:543)
// TRBs per endpoint plus a Link TRB in the last slot; each bulk buffer we
// allocate is exactly one page (4096 = 256 * 16), so the ring fits with no
// extra allocation.  UPDATETRANSFER takes no TRB address, so the core walks
// this ring by itself - see dwc3_ep_inc_trb() (gadget.c:165).
//
#define TRB_RING_NUM            256

#define EP0_OUT                  0
#define EP0_IN                   1
#define EP_BULK_OUT              2   // logical endpoint address 0x01
#define EP_BULK_IN               3   // logical endpoint address 0x81

#define MAX_PACKET_EP0           64
#define MAX_PACKET_BULK          512

//
// The transport hands us whole fastboot packets; a command is well under a
// kilobyte but a `download:` payload arrives in one bulk OUT transfer that
// can be arbitrarily long, so a fairly large ring buffer is used.
//
#define RX_CHUNK_SIZE            (64 * 1024)
#define TX_STATIC_BUF_SIZE       4096

#define EP0_DATA_MAX             256

#define POLL_INTERVAL_10MS       100000ULL   // 100ns units
#define TX_TIMEOUT_US            5000000ULL
//
// The kernel's budget, gadget.c:386-391: 5000 iterations of udelay(10) =
// 50ms, then pr_err("[USB.DBG] cmd:%x reg:%x time out!").  It is deliberately
// short: a wedged DEPCMD is unrecoverable there - dwc3_send_gadget_ep_cmd()
// returns -ETIMEDOUT, DEVICE_EVENT_CMD_TMO only prints, and no path resets
// the core - so spending 3s (300000 x 10us, the old diagnostic value) on it
// just turns a 50ms failure into a 3s stall that reads as a hang and makes
// hot-plug look broken.
//
#define CMD_TIMEOUT_LOOPS        5000
//
// 80% of that budget.  A completion that eats it points at clocking rather
// than at a wedge, which is the distinction the log exists to make.
//
#define CMD_SLOW_THRESHOLD       4000        // >40ms: print a slow-completion line

//
// ---------------------------------------------------------------------------
// Local types
// ---------------------------------------------------------------------------
//
#pragma pack(1)
typedef struct {
  UINT32    Bpl;
  UINT32    Bph;
  UINT32    Size;
  UINT32    Ctl;
} DWC3_TRB;
#pragma pack()

typedef enum {
  Ep0PhaseSetup = 0,
  Ep0PhaseData,
  Ep0PhaseStatus
} EP0_PHASE;

typedef enum {
  UsbStateDefault = 0,
  UsbStateAddress,
  UsbStateConfigured
} USB_GADGET_STATE;

//
#define KIRIN_SERIAL_STRING_INDEX  1
#define MAX_CONFIGURATIONS  8
#define MAX_HOST_HANDLES       4

// Protocol entry points and other cross-unit prototypes.
//
//
// Forward declarations (protocol members and mutually recursive handlers).
//
EFI_STATUS
EFIAPI
KirinUsbDeviceStart (
  IN USB_DEVICE_DESCRIPTOR   *DeviceDescriptor,
  IN VOID                    **Descriptors,
  IN USB_DEVICE_RX_CALLBACK  RxCallback,
  IN USB_DEVICE_TX_CALLBACK  TxCallback
  );

EFI_STATUS
EFIAPI
KirinUsbDeviceSend (
  IN UINT8       EndpointIndex,
  IN UINTN       Size,
  IN CONST VOID  *Buffer
  );

EFI_STATUS
EFIAPI
KirinUsbDeviceStop (
  VOID
  );

EFI_STATUS
EFIAPI
KirinUsbDeviceIdle (
  VOID
  );

EFI_STATUS
EFIAPI
KirinUsbDeviceSetRxLength (
  IN UINTN  Bytes
  );

VOID
ArmBulkOut (
  VOID
  );


//
// Cross-unit data.
//
extern USB_DEVICE_PROTOCOL     mUsbDevice;
extern KIRIN_USB_FUNCTION_PROTOCOL mKirinUsbFunction;
extern USB_DEVICE_DESCRIPTOR   mIdleDeviceDescriptor;
extern VOID  *mIdleConfigDescriptors[];
extern USB_DEVICE_RX_CALLBACK  mRxCallback;
extern USB_DEVICE_TX_CALLBACK  mTxCallback;
extern USB_DEVICE_DESCRIPTOR   *mDeviceDescriptor;
extern USB_DEVICE_DESCRIPTOR   mDeviceDescriptorCopy;
extern UINTN                 mConfigDescriptorCount;
extern VOID                  *mConfigDescriptorArray[MAX_CONFIGURATIONS];
extern VOID                  **mConfigDescriptors;
extern BOOLEAN                 mInitialized;
extern BOOLEAN                 mDmaAllocated;
extern BOOLEAN                 mPollBusy;
extern UINT32                  mGadgetState;
extern BOOLEAN                 mConfigured;
extern EFI_HANDLE              mHostHandles[MAX_HOST_HANDLES];
extern UINTN                   mHostHandleCount;
extern CONST CHAR8             *mInitStage;
extern UINT32                  *mEventBuffer;
extern UINT8                   *mCtrlRequest;
extern UINT8                   *mEp0Trb;
extern UINT8                   *mBulkInTrb;
extern UINT8                   *mBulkOutTrb;
extern UINT8                   *mTxDmaBuf;
extern UINT8                   *mRxDmaBuf;
extern UINT8                   *mEp0DataBuf;
extern UINTN                   mEventLpos;
//
// DSTS.USBLNKST as last reported by a link-status-change event, so the
// "wake up before STARTTRANSFER while parked in U1/U2/U3" rule has the same
// input the kernel's dwc->link_state does.
//
extern UINT8                   mLinkState;
extern EP0_PHASE               mEp0Phase;
extern BOOLEAN                 mEp0ThreeStage;
extern BOOLEAN                 mEp0ExpectIn;
extern BOOLEAN                 mEp0NextStatus;
extern BOOLEAN                 mEp0Stalled;
extern BOOLEAN                 mEp0StallPending;
extern BOOLEAN                 mEp0Busy;
extern UINT32                  mEp0Resource;
extern BOOLEAN                 mEp0ResourceValid;
extern UINT8                   mEp0ResourceEp;
extern USB_DEVICE_REQUEST      mSetup;
extern UINT8                   *mEp0Data;
extern UINTN                   mEp0DataLen;
//
// EP0 max packet in force at the negotiated speed, captured by the CONNDONE
// handler the way dwc3_gadget_conndone_interrupt() captures it before it
// re-issues SETEPCONFIG.  mEp0Zero is req->zero from composite.c:1982 -
// "armed length < wLength" - which is what tells dwc3_ep0_complete_data()
// (ep0.c) that the host has not yet seen the end of a control IN whose length
// is an exact multiple of the max packet size.
//
extern UINT32                  mEp0MaxPacket;
extern BOOLEAN                 mEp0Zero;
extern BOOLEAN                 mBulkInBusy;
extern UINT32                  mBulkInResource;
extern UINTN                   mBulkInArmedLen;
extern UINT32                  mBulkInTaken;
extern BOOLEAN                 mBulkOutBusy;
extern UINT32                  mBulkOutResource;
extern UINTN                   mBulkOutArmedLen;
extern UINT8                   mBulkInTrbEnq;
extern UINT8                   mBulkInTrbDeq;
extern UINT8                   mBulkOutTrbEnq;
extern UINT8                   mBulkOutTrbDeq;
extern UINT32                  mBulkOutPackets;
extern UINT32                  mBulkOutBytes;
extern UINTN                   mBulkOutRemaining;
extern UINT32                  mBulkXferEventCount;
extern UINT32                  mDeviceEventCount;
extern UINT32                  mUnknownIrqCount;
extern CONST CHAR8             *mEpCmdFailReason;
extern UINT32                  mBulkNoDataCount;
extern EFI_EVENT               mPollTimer;
extern UINT8                   mStatusBuf[2];
extern UINT8                   mMiscBuf[1];
extern UINT8                   mStringZero[4];
extern UINT8                   mStringEmpty[2];
extern UINT8                   mSerialString[26];
extern UINT8                   mQualifierBuf[10];
//
// Cross-unit helpers.
//
EFI_STATUS
AllocateDmaBuffers (
  VOID
  );
VOID
ArmBulkOut (
  VOID
  );
EFI_STATUS
ArmTransfer (
  IN  UINT8    EpNum,
  IN  UINT8    *Trb,
  IN  UINT32   ResourceIndex,
  OUT UINT32   *ResolvedResource
  );
VOID
ConnectHostControllers (
  VOID
  );
UINT32
DepRead (
  IN UINT8   EpNum,
  IN UINT32  Offset
  );
VOID
DepWrite (
  IN UINT8   EpNum,
  IN UINT32  Offset,
  IN UINT32  Value
  );
VOID
DisconnectHostControllers (
  VOID
  );
UINT32
DwRead (
  IN UINT32  Offset
  );
VOID
DwWrite (
  IN UINT32  Offset,
  IN UINT32  Value
  );
EFI_STATUS
Dwc3CoreInit (
  VOID
  );
VOID
Dwc3RunStop (
  IN BOOLEAN  Run
  );
VOID
Dwc3WakeupIfNeeded (
  VOID
  );
EFI_STATUS
EnableEndpoint (
  IN UINT8   EpNum,
  IN UINT32  EpType,
  IN UINT32  MaxPacket,
  IN BOOLEAN IsIn
  );
VOID
EndTransfer (
  IN UINT8  EpNum,
  IN UINT32 ResourceIndex
  );
VOID
Ep0ArmSetup (
  VOID
  );
VOID
Ep0Stall (
  VOID
  );
VOID
Ep0StartData (
  VOID
  );
VOID
Ep0StartStatus (
  IN UINT8  EpNum
  );
VOID
Ep0StartZlp (
  VOID
  );
EFI_STATUS
EpCmd (
  IN  UINT8    EpNum,
  IN  UINT32   Cmd,
  IN  UINT32   Param0,
  IN  UINT32   Param1,
  IN  UINT32   Param2,
  OUT UINT32   *CmdReg OPTIONAL
  );
VOID
HandleBulkOutComplete (
  VOID
  );
VOID
HandleStandardSetup (
  VOID
  );
VOID
InitTrbRing (
  IN UINT8   *Ring,
  OUT UINT8  *Enq,
  OUT UINT8  *Deq
  );
VOID
PollEvents (
  VOID
  );
VOID
EFIAPI
PollTimerNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  );
VOID
PrepareTrb (
  OUT DWC3_TRB  *Trb,
  IN  UINT64    Buffer,
  IN  UINT32    Length,
  IN  UINT32    TrbCtl,
  IN  BOOLEAN   Last
  );
EFI_STATUS
SetEpConfig (
  IN UINT8   EpNum,
  IN UINT32  EpType,      // 0 control, 1 isoc, 2 bulk, 3 interrupt
  IN UINT32  MaxPacket,
  IN BOOLEAN IsIn,
  IN BOOLEAN Modify
  );
VOID
SetPrtCapHost (
  VOID
  );
EFI_STATUS
StartConfig (
  VOID
  );
EFI_STATUS
StartTransfer (
  IN  UINT8    EpNum,
  IN  UINT8    *Trb,
  OUT UINT32   *ResourceIndex
  );
UINT64
TrbDma (
  IN UINT8   *Trb
  );
VOID
TrbInc (
  IN OUT UINT8  *Index
  );
EFI_STATUS
UpdateTransfer (
  IN  UINT8    EpNum,
  IN  UINT32   ResourceIndex,
  OUT UINT32   *CmdReg OPTIONAL
  );

//
// Prototypes for cross-unit helpers discovered after the split.
//
UINT8 *
TrbSlot (
  IN UINT8   *Ring,
  IN UINT8   Index
  );

#endif // KIRIN_USB_DEVICE_INTERNAL_H_
