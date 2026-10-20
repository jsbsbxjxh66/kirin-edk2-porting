#include "KirinUsbDeviceInternal.h"

//
// ---------------------------------------------------------------------------
// USB_DEVICE_PROTOCOL
// ---------------------------------------------------------------------------
//

//
// Give every transfer resource back to the hardware.  EndTransfer() is what
// fixes the "invalid transfer resource" status 1 that left endpoints holding
// their single resource forever.
//
STATIC
VOID
DropTransfers (
  VOID
  )
{
  if (mBulkInResource != 0) {
    EndTransfer (EP_BULK_IN, mBulkInResource);
    mBulkInResource = 0;
  }
  mBulkInBusy = FALSE;

  if (mBulkOutResource != 0) {
    EndTransfer (EP_BULK_OUT, mBulkOutResource);
    mBulkOutResource = 0;
  }
  mBulkOutBusy = FALSE;

  if (mEp0ResourceValid) {
    EndTransfer (mEp0ResourceEp, mEp0Resource);
    mEp0Resource      = 0;
    mEp0ResourceValid = FALSE;
  }
  mEp0Busy = FALSE;
}

STATIC
VOID
ApplyGadgetState (
  VOID
  )
{
  mGadgetState      = UsbStateDefault;
  mConfigured       = FALSE;
  mEp0Phase         = Ep0PhaseSetup;
  mEp0ThreeStage    = FALSE;
  mEp0ExpectIn      = FALSE;
  mEp0NextStatus    = FALSE;
  mEp0Stalled       = FALSE;
  mEp0StallPending  = FALSE;
}

//
// dwc3_gadget_pullup(1) -> dwc3_gadget_restart() (gadget.c:1729).  Teardown
// alone is not enough: without the core re-init the endpoint command engine
// keeps the state it had for the previous function, DEPSTARTCFG is already
// consumed, and the next SETUP command hangs with CMDACT stuck set (the
// "ep0 cmd 0x6 timed out, reg 0x00000406" failure).  Reproduce the kernel's
// __dwc3_gadget_stop -> dwc3_core_init -> event_buffers_setup ->
// __dwc3_gadget_start sequence exactly, then let the caller run_stop(1).
//
STATIC
EFI_STATUS
GadgetRestart (
  VOID
  )
{
  UINT32    Reg;
  EFI_TPL   OldTpl;
  EFI_STATUS Status;

  gBS->SetTimer (mPollTimer, TimerCancel, 0);

  //
  // PollTimerNotify is dispatched at TPL_CALLBACK as soon as TPL drops below
  // it, and SetTimer(TimerCancel) does NOT remove a notification that is
  // already sitting in the event queue - so PollEvents() can run in the middle
  // of the teardown below and issue an endpoint command against a block that
  // is being pulsed with GCTL.CORESOFTRESET.  That leaves CMDACT set, and the
  // very next StartConfig()/SetEpConfig() reports "cmd engine stuck".  It is
  // also why a fastboot or hot-plug entry only failed *sometimes*: it depended
  // on whether the 10 ms tick landed inside this window.  Holding TPL at
  // TPL_CALLBACK for the whole window is the DXE equivalent of the dwc->lock
  // spinlock the kernel holds across dwc3_gadget_interrupt().
  //
  OldTpl = gBS->RaiseTPL (TPL_CALLBACK);

  //
  // __dwc3_gadget_stop(): dwc3_gadget_disable_irq() writes DEVTEN = 0 and
  // masks the event buffer, then both EP0 halves are pulled out of DALEPENA
  // so the next DEPSTARTCFG round starts clean.
  //
  DwWrite (DWC3_DEVTEN, 0);
  Reg = DwRead (DWC3_GEVNTSIZ0);
  DwWrite (DWC3_GEVNTSIZ0, Reg | DWC3_GEVNTSIZ_INTMASK);

  DropTransfers ();
  DwWrite (DWC3_DALEPENA, 0);

  ApplyGadgetState ();

  //
  // pullup(0) holds RUNSTOP low and then waits ~60ms for the event buffer to
  // drain (gadget.c:1843).  Our buffer is zeroed by Dwc3CoreInit() below, so
  // the wait is only there to let in-flight bus-reset / connect events finish
  // being written before GEVNTCOUNT is reset - otherwise they are replayed
  // into the new session as a spurious "connected at HIGH speed".
  //
  Dwc3RunStop (FALSE);
  MicroSecondDelay (60000);

  Status = Dwc3CoreInit ();

  //
  // Anything queued while TPL was held dispatches here, against a core that is
  // fully configured but not yet pulled up - PollEvents() then finds
  // GEVNTCOUNT 0 and returns immediately.
  //
  gBS->RestoreTPL (OldTpl);
  return Status;
}

EFI_STATUS
EFIAPI
KirinUsbDeviceStart (
  IN USB_DEVICE_DESCRIPTOR   *DeviceDescriptor,
  IN VOID                    **Descriptors,
  IN USB_DEVICE_RX_CALLBACK  RxCallback,
  IN USB_DEVICE_TX_CALLBACK  TxCallback
  )
{
  EFI_STATUS  Status;
  UINTN       Index;

  if ((DeviceDescriptor == NULL) || (Descriptors == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (mInitialized) {
    //
    // Another function is live: dwc3_gadget_pullup(1) restarts the whole
    // core before re-attaching, which also forces a physical disconnect so
    // the host re-reads the new interface descriptor.
    //
    Status = GadgetRestart ();
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "KirinUsbDevice: controller restart failed at %a: %r\n",
        mInitStage,
        Status
        ));
      mInitialized = FALSE;
      return Status;
    }
  } else {
    //
    // Take the controller away from xHCI and from the host-role VBUS boost
    // before touching it, so nothing races the soft reset below.
    //
    DisconnectHostControllers ();
    KirinHi6523DisableVbusBoost ();

    Status = AllocateDmaBuffers ();
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "KirinUsbDevice: DMA buffers failed: %r\n", Status));
      return Status;
    }

    Status = Dwc3CoreInit ();
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "KirinUsbDevice: controller init failed at %a: %r\n",
        mInitStage,
        Status
        ));
      return Status;
    }

    if (mPollTimer == NULL) {
      Status = gBS->CreateEvent (
                      EVT_TIMER | EVT_NOTIFY_SIGNAL,
                      TPL_CALLBACK,
                      PollTimerNotify,
                      NULL,
                      &mPollTimer
                      );
      if (EFI_ERROR (Status)) {
        DEBUG ((
          DEBUG_ERROR,
          "KirinUsbDevice: CreateEvent failed: %r\n",
          Status
          ));
        return Status;
      }
    }

    mInitialized = TRUE;
  }

  //
  // Snapshot the caller's device descriptor so it can be read back for the
  // qualifier query and so StrSerialNumber can be patched below without
  // writing into the caller's storage.
  //
  CopyMem (&mDeviceDescriptorCopy, DeviceDescriptor, sizeof (USB_DEVICE_DESCRIPTOR));
  mDeviceDescriptor = &mDeviceDescriptorCopy;

  if (mDeviceDescriptor->StrSerialNumber == 0) {
    mDeviceDescriptor->StrSerialNumber = KIRIN_SERIAL_STRING_INDEX;
  }

  //
  // Snapshot the caller's descriptor array before this call returns.
  // USB_DEVICE_PROTOCOL only promises Descriptors is valid for the duration
  // of Start(), and the EmbeddedPkg fastboot transport passes
  // (VOID **)&Responses where Responses is a local of its own Start
  // function - keeping the double pointer meant mConfigDescriptors[0] read
  // reused stack long before the host asked for configuration 0.  That is
  // precisely where enumeration used to die: device descriptor (a static)
  // came back fine, the configuration request got a stall or a wild pointer,
  // and the host reset the bus and started over with a new address.
  //
  mConfigDescriptorCount = 0;
  for (Index = 0;
       (Index < DeviceDescriptor->NumConfigurations)
       && (Index < MAX_CONFIGURATIONS)
       && (Descriptors[Index] != NULL);
       Index++)
  {
    mConfigDescriptorArray[Index] = Descriptors[Index];
    mConfigDescriptorCount++;
  }
  mConfigDescriptors = mConfigDescriptorArray;

  if (mConfigDescriptorCount == 0) {
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: caller supplied no usable configuration descriptors\n"
      ));
    return EFI_INVALID_PARAMETER;
  }

  mRxCallback = RxCallback;
  mTxCallback = TxCallback;
  ApplyGadgetState ();

  //
  // Attach: this is what makes the host see a device on the port.
  //
  Dwc3RunStop (TRUE);

  gBS->SetTimer (mPollTimer, TimerPeriodic, POLL_INTERVAL_10MS);

  DEBUG ((DEBUG_ERROR,
    "KirinUsbDevice: gadget started, vid %04x pid %04x\n",
    mDeviceDescriptor->IdVendor,
    mDeviceDescriptor->IdProduct
    ));

  return EFI_SUCCESS;
}

//
// ---------------------------------------------------------------------------
// KIRIN_USB_FUNCTION_PROTOCOL
// ---------------------------------------------------------------------------
//

EFI_STATUS
EFIAPI
KirinUsbDeviceStop (
  VOID
  )
{
  EFI_TPL  OldTpl;

  if (!mInitialized) {
    return EFI_SUCCESS;
  }

  gBS->SetTimer (mPollTimer, TimerCancel, 0);

  //
  // Same window GadgetRestart() closes: a PollTimerNotify already queued
  // would otherwise run PollEvents() while the core is being stopped and hand
  // DropTransfers()/Dwc3RunStop() a half-processed event stream.
  //
  OldTpl = gBS->RaiseTPL (TPL_CALLBACK);

  DropTransfers ();
  Dwc3RunStop (FALSE);

  gBS->RestoreTPL (OldTpl);

  //
  // Port capability back to HOST, exactly what KirinDwc3Dxe's
  // Dwc3SetHostMode() programmed at boot, then let XhciDxe bind again.
  //
  SetPrtCapHost ();
  ConnectHostControllers ();
  KirinHi6523EnableVbusBoost ();

  mInitialized      = FALSE;
  mConfigured       = FALSE;
  mGadgetState      = UsbStateDefault;
  mEp0Phase         = Ep0PhaseSetup;

  DEBUG ((DEBUG_ERROR, "KirinUsbDevice: handed back to host mode\n"));
  return EFI_SUCCESS;
}

//
// Default gadget: device mode with no function bound, so whatever the host
// sends is dropped.  Reachable only from the boot menu - the usb-otg entry,
// i.e. KIRIN_USB_FUNCTION_PROTOCOL->Idle() - because the port now boots and
// stays in host (OTG) mode until something explicitly asks for a gadget.
//
EFI_STATUS
EFIAPI
KirinUsbDeviceIdle (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = KirinUsbDeviceStart (
             &mIdleDeviceDescriptor,
             mIdleConfigDescriptors,
             NULL,
             NULL
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: idle gadget failed: %r\n",
      Status
      ));
    return Status;
  }

  DEBUG ((DEBUG_ERROR, "KirinUsbDevice: idle gadget up\n"));
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
KirinUsbDeviceSetRxLength (
  IN UINTN  Bytes
  )
{
  //
  // Replacing, not accumulating: a second "download:" supersedes whatever
  // the previous one was still owed.  Only the next arm is affected - one
  // is already in flight at RX_CHUNK_SIZE - and by the time that arm
  // completes the count has already been updated, so no arm can be sized
  // against a stale number.
  //
  mBulkOutRemaining = Bytes;

  DEBUG ((
    DEBUG_ERROR,
    "KirinUsbDevice: rx expected %u bytes\n",
    (UINT32)Bytes
    ));
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
KirinUsbDeviceSend (
  IN UINT8       EndpointIndex,
  IN UINTN       Size,
  IN CONST VOID  *Buffer
  )
{
  UINT8   *Tx;
  UINT8   *Slot;
  UINT8   SlotIndex;
  UINTN   TempPages;
  UINTN   Retry;
  UINTN   StartLoops;
  UINTN   ElapsedMs;
  UINT32  Reg;
  UINT32  Taken;
  BOOLEAN Freed;
  EFI_STATUS Status;

  if ((Buffer == NULL) && (Size != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!mConfigured) {
    DEBUG ((DEBUG_ERROR, "KirinUsbDevice: send before SET_CONFIGURATION\n"));
    return EFI_DEVICE_ERROR;
  }

  //
  // Two overlapping bulk IN arms would put two live TRBs in the ring while
  // completion only ever reads the dequeue slot, so the second transfer would
  // never be seen and this call would spin for the full timeout.  Refuse it
  // loudly instead - and the print says whether a response was ever attempted.
  //
  if (mBulkInBusy) {
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: send %u while previous still in flight\n",
      (UINT32)Size
      ));
    return EFI_DEVICE_ERROR;
  }

  //
  // Printed every time, not just the first: fastboot only ever sends a handful
  // of small responses (DATA..., OKAY, FAIL), and whether the OKAY after the
  // download phase was even attempted is exactly the question we need answered.
  //
  DEBUG ((
    DEBUG_ERROR,
    "KirinUsbDevice: device -> host %u bytes\n",
    (UINT32)Size
    ));

  Tx     = NULL;
  Freed  = FALSE;
  TempPages = 0;

  if (Size <= TX_STATIC_BUF_SIZE) {
    Tx = mTxDmaBuf;
  } else {
    TempPages = EFI_SIZE_TO_PAGES (Size);
    Tx = AllocateAlignedPages (TempPages, TempPages);
    if (Tx == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    Freed = TRUE;
  }

  if (Size != 0) {
    CopyMem (Tx, Buffer, Size);
    WriteBackDataCacheRange (Tx, Size);
  }

  //
  // Same ring discipline as ArmBulkOut(): one TRB per slot, cursor advanced
  // before the command, address only meaningful for the first STARTTRANSFER.
  //
  SlotIndex = mBulkInTrbEnq;
  Slot      = TrbSlot (mBulkInTrb, SlotIndex);
  PrepareTrb (
    (DWC3_TRB *)Slot,
    (UINT64)(UINTN)Tx,
    (UINT32)Size,
    DWC3_TRBCTL_NORMAL,
    TRUE
    );
  TrbInc (&mBulkInTrbEnq);
  mBulkInArmedLen = Size;

  //
  // Same STARTTRANSFER-vs-UPDATETRANSFER choice as ArmBulkOut(), and the same
  // bookkeeping repair.  This is the fix for "bulk IN arm failed": ep3 keeps
  // both its transfer resource and DWC3_EP_BUSY across an XFERINPROGRESS
  // completion (gadget.c:2523 and 2458-2461), so the kernel issues
  // UPDATETRANSFER here - never a second STARTTRANSFER, and never
  // ENDTRANSFER, whose only callers are disable/dequeue/isoc paths
  // (gadget.c:715, 1278, 1397, 2427, 2823).  The old release-then-restart
  // sequence issued ENDTRANSFER against a transfer the core had already
  // finished; the command never cleared CMDACT, every later DEPCMD write was
  // ignored (the retry read back 0x00000406), and that is what reported
  // "bulk IN arm failed".
  //
  Status = ArmTransfer (
             EP_BULK_IN,
             Slot,
             mBulkInResource,
             &mBulkInResource
             );

  if (EFI_ERROR (Status)) {
    //
    // Nothing was started, so hand the slot back to keep enqueue/dequeue
    // aligned with the core (see ArmBulkOut()).
    //
    mBulkInTrbEnq = SlotIndex;
    Reg = DepRead (EP_BULK_IN, DWC3_DEPCMD);
    //
    // st/act decoded so one photo is conclusive: act 1 = CMDACT wedge (a
    // previous command never completed), st 1 = no transfer resource,
    // st 0 + act 0 = the write never landed at all.
    //
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: bulk IN arm failed (%a rsc %u DEPCMD 0x%08x st %u act %u)\n",
      (mEpCmdFailReason != NULL) ? mEpCmdFailReason : "no-epcmd",
      mBulkInResource,
      Reg,
      DWC3_DEPCMD_STATUS (Reg),
      ((Reg & DWC3_DEPCMD_CMDACT) != 0) ? 1 : 0
      ));
    if (Freed) {
      FreePages (Tx, TempPages);
    }
    return EFI_DEVICE_ERROR;
  }
  mBulkInBusy = TRUE;
  StartLoops  = TX_TIMEOUT_US / 10;

  for (Retry = StartLoops; Retry > 0; Retry--) {
    PollEvents ();
    if (!mBulkInBusy) {
      break;
    }
    MicroSecondDelay (10);
  }

  //
  // A single Send never used to take anywhere near this long, so anything
  // past a quarter second means the round trip is creeping - print it with
  // the message size so a slow command can be seen developing rather than
  // only discovered as a 5s timeout.
  //
  ElapsedMs = ((StartLoops - Retry) * 10) / 1000;
  if ((ElapsedMs > 250) && !mBulkInBusy) {
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: bulk IN send %u bytes slow (%u ms)\n",
      (UINT32)Size,
      (UINT32)ElapsedMs
      ));
  }

  if (mBulkInBusy) {
    //
    // The residual is the whole diagnosis: it says how many of the armed bytes
    // the host actually took, and that decides which teardown is safe.
    //
    //   host took 0  -> the transfer is still live, the core owns it, and an
    //                   ENDTRANSFER is the ordinary dwc3_stop_active_transfer()
    //                   the kernel issues for a dequeue.  It completes, because
    //                   the wedge only ever came from ENDTRANSFERing a transfer
    //                   the core had already finished (see EpCmd()).
    //
    //   host took N>0 -> the transfer finished and we simply never saw the
    //                   event.  ENDTRANSFER here is exactly the wedge: skip
    //                   the slot and move on, no command issued at all.
    //
    // Either way the endpoint must come back, otherwise one slow host costs
    // every command after it (that is what turned into "no link").
    //
    Slot = TrbSlot (mBulkInTrb, mBulkInTrbDeq);
    InvalidateDataCacheRange (Slot, DWC3_TRB_SIZE);
    Reg      = ((DWC3_TRB *)Slot)->Size & 0x00FFFFFFu;
    Taken    = (Reg > mBulkInArmedLen) ? 0 : (mBulkInArmedLen - Reg);
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: bulk IN timed out (%u ms, armed %u, host took %u, "
      "rsc %u dalepena 0x%08x depcmd 0x%08x)\n",
      (UINT32)ElapsedMs,
      (UINT32)mBulkInArmedLen,
      (UINT32)Taken,
      mBulkInResource,
      DwRead (DWC3_DALEPENA),
      DepRead (EP_BULK_IN, DWC3_DEPCMD)
      ));

    if (Taken == 0) {
      if (mBulkInResource != 0) {
        EndTransfer (EP_BULK_IN, mBulkInResource);
        mBulkInResource = 0;
      }
    }
    //
    // Dead either way - hand the slot back so enqueue and dequeue agree again.
    //
    TrbInc (&mBulkInTrbDeq);
    mBulkInBusy = FALSE;
    if (Freed) {
      FreePages (Tx, TempPages);
    }
    return EFI_TIMEOUT;
  }

  if (mBulkInTaken == Size) {
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: bulk IN done %u bytes slot %u\n",
      (UINT32)Size,
      (UINT32)SlotIndex
      ));
  } else {
    //
    // The host left bytes on the table.  The requested size next to the
    // delivered size on one line is the whole diagnosis - and it is the case
    // that would otherwise be invisible, because the old success line printed
    // the request as if it had landed.
    //
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: bulk IN done %u/%u bytes slot %u (short)\n",
      (UINT32)mBulkInTaken,
      (UINT32)Size,
      (UINT32)SlotIndex
      ));
  }

  if (Freed) {
    FreePages (Tx, TempPages);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
KirinUsbDeviceEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gUsbDeviceProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mUsbDevice
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gKirinUsbFunctionProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mKirinUsbFunction
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Deliberately no EndOfDxe hook: an earlier revision called
  // KirinUsbDeviceIdle() from here, which pulled a gadget up as soon as DXE
  // dispatch finished - before the boot menu was even drawn - so plugging a
  // cable into a PC produced an unknown device nobody had asked for.  The
  // port now keeps the host (OTG) role KirinDwc3Dxe's Dwc3SetHostMode() and
  // XhciDxe leave it in at boot, and only a usb-mode menu entry puts it into
  // device mode.  That also keeps the old ordering requirement satisfied:
  // the first Start()/Idle() is reachable only from the menu, i.e. long after
  // UsbSubsystemInit() (PHY, combophy clock mux, TCA) has run and XhciDxe has
  // bound, which is what DisconnectHostControllers() needs to take away.
  //
  return EFI_SUCCESS;
}
