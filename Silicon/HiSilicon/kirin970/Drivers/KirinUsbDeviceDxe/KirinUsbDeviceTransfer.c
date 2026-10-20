#include "KirinUsbDeviceInternal.h"

//
// ---------------------------------------------------------------------------
// Transfer engine
// ---------------------------------------------------------------------------
//
VOID
PrepareTrb (
  OUT DWC3_TRB  *Trb,
  IN  UINT64    Buffer,
  IN  UINT32    Length,
  IN  UINT32    TrbCtl,
  IN  BOOLEAN   Last
  )
{
  Trb->Bpl  = (UINT32)Buffer;
  Trb->Bph  = (UINT32)RShiftU64 (Buffer, 32);
  Trb->Size = DWC3_TRB_SIZE_LENGTH (Length);
  Trb->Ctl  = TrbCtl | DWC3_TRB_CTRL_HWO;

  //
  // CSP is a gadget-layer bit: dwc3_prepare_one_trb() ORs it unconditionally
  // (gadget.c:980).  EP0 never goes through that function - every control TRB
  // is built by dwc3_ep0_prepare_one_trb() (ep0.c:59), which sets only
  // HWO | ISP_IMI plus CHN, or IOC | LST when the TRB is not chained.
  // A descriptor read is normally a short packet (the host asks for 64 and
  // gets 18), and CSP on a short packet tells the core to keep fetching TRBs
  // after it - from a single, LST-terminated control TRB with nothing to
  // fetch.  The kernel path enumeration depends on has no such bit, so the
  // only defensible value here is the kernel's.
  //
  if (TrbCtl == DWC3_TRBCTL_NORMAL) {
    Trb->Ctl |= DWC3_TRB_CTRL_CSP;
  }

  if (Last) {
    Trb->Ctl |= DWC3_TRB_CTRL_IOC | DWC3_TRB_CTRL_ISP_IMI;

    //
    // dwc3_ep0_prepare_one_trb() (ep0.c:82) sets LST only for the last TRB
    // of a control transfer.  dwc3_prepare_one_trb() (gadget.c:987) never
    // sets LST for a NORMAL (bulk) TRB - it relies on CHN=0 alone to end the
    // transfer.  A non-zero LST on a bulk TRB is a silent, un-checked
    // deviation from the known-working kernel path, so keep it for control
    // only.
    //
    if (TrbCtl != DWC3_TRBCTL_NORMAL) {
      Trb->Ctl |= DWC3_TRB_CTRL_LST;
    }
  } else {
    Trb->Ctl |= DWC3_TRB_CTRL_CHN;
  }

  WriteBackDataCacheRange (Trb, DWC3_TRB_SIZE);
}

EFI_STATUS
StartTransfer (
  IN  UINT8    EpNum,
  IN  UINT8    *Trb,
  OUT UINT32   *ResourceIndex
  )
{
  EFI_STATUS Status;
  UINT32     CmdReg;
  UINT64     Dma;

  Dma   = TrbDma (Trb);
  Status = EpCmd (
             EpNum,
             DWC3_DEPCMD_STARTTRANSFER,
             (UINT32)RShiftU64 (Dma, 32),
             (UINT32)Dma,
             0,
             &CmdReg
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *ResourceIndex = DWC3_DEPCMD_GET_RSC_IDX (CmdReg);
  return EFI_SUCCESS;
}

/**
  Continue a transfer whose endpoint already owns a transfer resource.

  __dwc3_gadget_kick_transfer() (gadget.c:1129-1147) picks between the two
  commands on a single flag:

      starting = !(dep->flags & DWC3_EP_BUSY);
      if (starting)  cmd = STARTTRANSFER | PARAM(cmd_param);   // TRB in PAR0/1
      else           cmd = UPDATETRANSFER | PARAM(dep->resource_index);

  For bulk endpoints both DWC3_EP_BUSY (gadget.c:2458-2461) and
  dep->resource_index (gadget.c:2523-2525) survive completion, because the
  core never raises XFERCOMPLETE on them - dwc3_gadget_set_ep_config() only
  enables XFER_IN_PROGRESS_EN for non-control endpoints (gadget.c:599-603).
  "starting" is therefore false for every bulk transfer after the first, and
  the kernel issues UPDATETRANSFER with all three parameter words left at 0:
  the core picks the next TRB up from the ring itself, which is why the ring
  (InitTrbRing) exists.

  This is also why ENDTRANSFER must stay out of the bulk data path.  The only
  kernel callers of dwc3_stop_active_transfer() are endpoint disable, dequeue
  and isoc handling (gadget.c:715, 1278, 1397, 2427, 2823) - never a normal
  bulk re-arm - and issuing one against a transfer the core has already
  finished is what wedged CMDACT and produced "bulk IN arm failed".
**/
EFI_STATUS
UpdateTransfer (
  IN  UINT8    EpNum,
  IN  UINT32   ResourceIndex,
  OUT UINT32   *CmdReg OPTIONAL
  )
{
  return EpCmd (
           EpNum,
           DWC3_DEPCMD_UPDATETRANSFER | DWC3_DEPCMD_PARAM (ResourceIndex),
           0,
           0,
           0,
           CmdReg
           );
}

/**
  Arm one bulk transfer, choosing STARTTRANSFER or UPDATETRANSFER and
  repairing the bookkeeping if the two sides disagree.

  The decision itself is the kernel's: __dwc3_gadget_kick_transfer() starts a
  transfer when the endpoint owns no resource and updates it otherwise.  The
  recovery is the part the kernel does not need, because its disable path
  always runs dwc3_stop_active_transfer() and therefore never lets the
  software index drift from the hardware.  Ours does drift in two directions:

    - software says "none" but the core still owns one.  Happens if a bus
      reset or a re-configuration released our view of the state without the
      hardware releasing the resource.  STARTTRANSFER answers status 1
      ("there is no transfer resource available on the endpoint",
      gadget.c:268) and the core leaves the index it is holding in
      DEPCMD[22:16], which is exactly the field
      dwc3_gadget_ep_get_transfer_index() reads (gadget.h:98-104).

    - software says "index N" but the core no longer has it.  UPDATETRANSFER
      answers status 1 (DEPEVT_TRANSFER_NO_RESOURCE) and we fall back to a
      plain STARTTRANSFER with the TRB address.

  Either way the repair costs one extra command and never touches
  ENDTRANSFER, so the CMDACT wedge that caused "bulk IN arm failed" cannot be
  reached from here.
**/
EFI_STATUS
ArmTransfer (
  IN  UINT8    EpNum,
  IN  UINT8    *Trb,
  IN  UINT32   ResourceIndex,
  OUT UINT32   *ResolvedResource
  )
{
  EFI_STATUS Status;
  UINT32     Reg;
  UINT32     Found;

  *ResolvedResource = ResourceIndex;
  Reg = 0;

  if (ResourceIndex == 0) {
    Status = StartTransfer (EpNum, Trb, ResolvedResource);
    if (!EFI_ERROR (Status)) {
      return EFI_SUCCESS;
    }

    Reg    = DepRead (EpNum, DWC3_DEPCMD);
    Found  = DWC3_DEPCMD_GET_RSC_IDX (Reg);
    if ((DWC3_DEPCMD_STATUS (Reg) != 1) || (Found == 0)) {
      // Not "no resource available", or nothing reported to recover.  A real
      // failure - report it rather than guess.
      return Status;
    }

    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: ep%d owns rsc %u already, updating instead\n",
      EpNum,
      Found
      ));
    *ResolvedResource = Found;
    Status = UpdateTransfer (EpNum, Found, &Reg);
    if (EFI_ERROR (Status)) {
      *ResolvedResource = 0;
    }
    return Status;
  }

  Status = UpdateTransfer (EpNum, ResourceIndex, &Reg);
  if (!EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  if (DWC3_DEPCMD_STATUS (Reg) != 1) {
    return Status;
  }

  DEBUG ((
    DEBUG_ERROR,
    "KirinUsbDevice: ep%d rsc %u gone, starting fresh\n",
    EpNum,
    ResourceIndex
    ));
  *ResolvedResource = 0;
  return StartTransfer (EpNum, Trb, ResolvedResource);
}

VOID
EndTransfer (
  IN UINT8  EpNum,
  IN UINT32 ResourceIndex
  )
{
  //
  // There used to be an "if (ResourceIndex == 0) return;" here.  Resource
  // index 0 is a real index on this core (EP0-OUT's, see mEp0ResourceValid),
  // and the kernel passes it too - dwc3_stop_active_transfer() just forwards
  // dep->resource_index.  The guard made EP0 impossible to tear down: every
  // stale SETUP transfer kept its resource forever and every later
  // STARTTRANSFER on EP0 reported status 1 (DEPEVT_TRANSFER_NO_RESOURCE).
  // Callers now gate on mEp0ResourceValid (EP0) or != 0 (bulk, whose indices
  // are never 0) instead of on the value itself.
  //

  //
  // The resource index is a field of the DEPCMD word itself
  // (DWC3_DEPCMD_PARAM -> bits 22:16).  It is NOT DEPCMDPAR2: the core
  // ignores PAR2 for ENDTRANSFER, so passing it there makes the command
  // target resource 0, which the core rejects with "an invalid transfer
  // resource was specified" and leaves the transfer running.  The endpoint
  // keeps its only transfer resource forever and every later STARTTRANSFER
  // on it reports status 1.
  //
  //
  // HIPRI_FORCERM: dwc3_stop_active_transfer() ORs it in whenever force is
  // set (gadget.c:2697), and every one of its five call sites - disable (715),
  // dequeue (1278, 1397), isoc (1397, 2427), disconnect (2823) - passes
  // force=true, so it is on for every ENDTRANSFER the kernel issues.  It asks
  // the interconnect to force the in-flight transfer out; without it the core
  // can keep owning the resource, which is exactly the status 1
  // (DEPEVT_TRANSFER_NO_RESOURCE) on the next STARTTRANSFER that ArmTransfer()
  // then has to repair.
  //
  EpCmd (
    EpNum,
    DWC3_DEPCMD_ENDTRANSFER | DWC3_DEPCMD_HIPRI_FORCERM | DWC3_DEPCMD_CMDIOC
    | DWC3_DEPCMD_PARAM (ResourceIndex),
    0,
    0,
    0,
    NULL
    );

  //
  // dwc3_stop_active_transfer() gives the core 100us after an EndTransfer
  // with CMDIOC before the resource may be handed out again (databook
  // 6.3.2.5).  Keep it: it is a fixed cost on a path that now only runs on
  // ep disable/timeout, and on a DWC_usb31 core the GUCTL2[14] polling aid
  // does not exist (Dwc3CoreInit() prints GSNPSID, which says which we have).
  //
  MicroSecondDelay (100);
}
