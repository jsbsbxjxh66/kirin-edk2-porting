#include "KirinUsbDeviceInternal.h"

//
// ---------------------------------------------------------------------------
// Low level endpoint command engine
// ---------------------------------------------------------------------------
//
EFI_STATUS
EpCmd (
  IN  UINT8    EpNum,
  IN  UINT32   Cmd,
  IN  UINT32   Param0,
  IN  UINT32   Param1,
  IN  UINT32   Param2,
  OUT UINT32   *CmdReg OPTIONAL
  )
{
  UINTN  Retry;
  UINT32 Reg;
  UINT32 CmdStatus;

  //
  // Cleared here, not on success: ArmTransfer() may call EpCmd() twice
  // (failed START repaired by a second command), and the caller only ever
  // looks at this after the call that actually produced the error.
  //
  mEpCmdFailReason = NULL;

  //
  // Fail fast when a previous command on this endpoint never completed.
  //
  // What happened on ep3: an ENDTRANSFER was issued against a transfer the
  // core had already finished.  The register kept our exact word -
  // 0x00110508 = PARAM(17) | CMDIOC | CMDACT | cmd 8 - with the status field
  // still 0, i.e. the core never ran it, and every later command written to
  // that DEPCMD register was ignored (the retry read back 0x00000406).  So
  // one bad teardown turned into "bulk IN arm failed" plus a multi-second
  // stall inside SET_CONFIGURATION, which is what made hot-plug look broken.
  //
  // The bulk data path no longer issues ENDTRANSFER at all (see
  // UpdateTransfer()), and GUCTL2[14] is now set so the ones that remain -
  // EP0 and the TX timeout path - can poll CMDACT to completion.  This check
  // is left in as the guard that stops the next such mistake from spending
  // 3s per command instead of failing immediately.
  //
  Reg = DepRead (EpNum, DWC3_DEPCMD);
  if ((Reg & DWC3_DEPCMD_CMDACT) != 0) {
    if (CmdReg != NULL) {
      *CmdReg = Reg;
    }
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: ep%d cmd engine stuck (reg 0x%08x)\n",
      EpNum,
      Reg
      ));
    mEpCmdFailReason = "cmdact-stuck";
    return EFI_DEVICE_ERROR;
  }

  //
  // dwc3_send_gadget_ep_cmd() (gadget.c:337-347) wakes the link up before it
  // issues a STARTTRANSFER while the link is parked in U1/U2/U3, and does so
  // for no other command.  This is where the kernel puts it: after the SUSPHY
  // check that we do not need (SUSPHY is left clear for good) and before the
  // parameter writes.
  //
  if (DWC3_DEPCMD_CMD (Cmd) == DWC3_DEPCMD_STARTTRANSFER) {
    Dwc3WakeupIfNeeded ();
  }

  DepWrite (EpNum, DWC3_DEPCMDPAR0, Param0);
  DepWrite (EpNum, DWC3_DEPCMDPAR1, Param1);
  DepWrite (EpNum, DWC3_DEPCMDPAR2, Param2);
  DepWrite (EpNum, DWC3_DEPCMD, Cmd | DWC3_DEPCMD_CMDACT);
  //
  // dwc3_send_gadget_ep_cmd() does an rmb() between the command write and
  // the first read-back of CMDACT.  Without it the read can race ahead of
  // the command write and either fail to observe CMDACT at all (false
  // success) or observe a stale value.
  //
  MemoryFence ();

  for (Retry = CMD_TIMEOUT_LOOPS; Retry > 0; Retry--) {
    Reg = DepRead (EpNum, DWC3_DEPCMD);
    if ((Reg & DWC3_DEPCMD_CMDACT) == 0) {
      if (CmdReg != NULL) {
        *CmdReg = Reg;
      }
        CmdStatus = DWC3_DEPCMD_STATUS (Reg);
        //
        // dwc3_send_gadget_ep_cmd() (gadget.c:370-417) only turns a non-zero
        // status into an error for the two documented values -
        // DEPEVT_TRANSFER_NO_RESOURCE(1) and DEPEVT_TRANSFER_BUS_EXPIRY(2) -
        // which its switch maps to -EINVAL / -EAGAIN.  Anything >=3 falls into
        // `default: dev_WARN("UNKNOWN cmd status")` and leaves ret at 0, i.e.
        // the kernel warns and carries on; dwc3_check_gadget_ep_cmd_status()
        // likewise only ever looks at STARTTRANSFER / SETTRANSFRESOURCE /
        // ENDTRANSFER.  Treating every non-zero status as a failure was our
        // own addition, and it is what turns a benign SETEPCONFIG status into
        // "conndone EP0 config failed" during enumeration.
        //
        if ((CmdStatus == DWC3_DEPCMD_ST_NO_RSC) ||
            (CmdStatus == DWC3_DEPCMD_ST_BUS_EXPIRY))
        {
          DEBUG ((
            DEBUG_ERROR,
            "KirinUsbDevice: ep%d cmd 0x%x status %d (reg 0x%08x)\n",
            EpNum,
            DWC3_DEPCMD_CMD (Cmd),
            CmdStatus,
            Reg
            ));
          mEpCmdFailReason = "rejected";
        } else if (CmdStatus != 0) {
          //
          // Kept at DEBUG_ERROR on purpose: RELEASE only passes EFI_D_ERROR
          // through, and "the core answered with a status it should not use"
          // is exactly the observation these logs exist for.
          //
          DEBUG ((
            DEBUG_ERROR,
            "KirinUsbDevice: ep%d cmd 0x%x status %d tolerated "
            "(reg 0x%08x)\n",
            EpNum,
            DWC3_DEPCMD_CMD (Cmd),
            CmdStatus,
            Reg
            ));
        }
        //
        // A completion that took >100ms is itself diagnostic: the kernel
        // finishes these within 50ms, so a slow success points at clocking
        // rather than a wedged engine.
        //
        if ((CMD_TIMEOUT_LOOPS - Retry) > CMD_SLOW_THRESHOLD) {
          DEBUG ((
            DEBUG_ERROR,
            "KirinUsbDevice: ep%d cmd 0x%x completed slow "
            "(%u loops left of %u)\n",
            EpNum,
            DWC3_DEPCMD_CMD (Cmd),
            (UINT32)Retry,
            (UINT32)CMD_TIMEOUT_LOOPS
            ));
        }
        return ((CmdStatus == DWC3_DEPCMD_ST_NO_RSC) ||
                (CmdStatus == DWC3_DEPCMD_ST_BUS_EXPIRY)) ?
               EFI_DEVICE_ERROR : EFI_SUCCESS;
    }
    MicroSecondDelay (10);
  }

  //
  // CMDACT never cleared.  The register read-back says whether the engine is
  // still busy, whether the command was rejected, and whether the write even
  // landed - all three distinguish a hung command from a blind write.
  //
  Reg = DepRead (EpNum, DWC3_DEPCMD);
  if (CmdReg != NULL) {
    *CmdReg = Reg;
  }
  DEBUG ((
    DEBUG_ERROR,
    "KirinUsbDevice: ep%d cmd 0x%x timed out (reg 0x%08x "
    "par0 0x%08x par1 0x%08x par2 0x%08x)\n",
    EpNum,
    DWC3_DEPCMD_CMD (Cmd),
    Reg,
    DepRead (EpNum, DWC3_DEPCMDPAR0),
    DepRead (EpNum, DWC3_DEPCMDPAR1),
    DepRead (EpNum, DWC3_DEPCMDPAR2)
    ));
  mEpCmdFailReason = "cmd-timeout";
  return EFI_TIMEOUT;
}

/**
  Assign one transfer resource to every hardware endpoint.  Called once per
  configuration round, right after DEPSTARTCFG(0).
**/
STATIC
EFI_STATUS
SetTransferResources (
  VOID
  )
{
  UINT32  HwParams3;
  UINT32  NumEps;
  UINT32  NumIn;
  UINT32  NumOut;
  UINT32  Index;
  UINT8   EpNum;

  HwParams3 = DwRead (DWC3_GHWPARAMS3);
  NumEps    = (HwParams3 >> 12) & 0x3F;
  NumIn     = (HwParams3 >> 18) & 0x1F;
  NumOut    = (NumEps > NumIn) ? (NumEps - NumIn) : 0;

  if ((NumOut == 0) || (NumIn == 0)) {
    DEBUG ((DEBUG_ERROR, "KirinUsbDevice: bad GHWPARAMS3 0x%x\n", HwParams3));
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((
    DEBUG_ERROR,
    "KirinUsbDevice: xfer %d out %d in (hwparams3 0x%08x)\n",
    NumOut,
    NumIn,
    HwParams3
    ));

  for (Index = 0; Index < NumOut; Index++) {
    EpNum = (UINT8)(Index * 2);
    if (EFI_ERROR (EpCmd (EpNum, DWC3_DEPCMD_SETTRANSFRESOURCE,
                          DWC3_DEPXFERCFG_NUM_XFER_RES (1), 0, 0, NULL))) {
      return EFI_DEVICE_ERROR;
    }
  }

  for (Index = 0; Index < NumIn; Index++) {
    EpNum = (UINT8)((Index * 2) + 1);
    if (EFI_ERROR (EpCmd (EpNum, DWC3_DEPCMD_SETTRANSFRESOURCE,
                          DWC3_DEPXFERCFG_NUM_XFER_RES (1), 0, 0, NULL))) {
      return EFI_DEVICE_ERROR;
    }
  }

  DEBUG ((
    DEBUG_ERROR,
    "KirinUsbDevice: after XFERCFG, dep0 DEPCMD 0x%08x dep1 DEPCMD 0x%08x\n",
    DepRead (EP0_OUT, DWC3_DEPCMD),
    DepRead (EP0_IN, DWC3_DEPCMD)
    ));

  return EFI_SUCCESS;
}

/**
  Begin a new endpoint configuration round (DEPSTARTCFG(0)) and give every
  hardware endpoint one transfer resource.
**/
EFI_STATUS
StartConfig (
  VOID
  )
{
  UINT32  Reg;

  if (EFI_ERROR (EpCmd (EP0_OUT, DWC3_DEPCMD_DEPSTARTCFG, 0, 0, 0, &Reg))) {
    return EFI_DEVICE_ERROR;
  }

  //
  // 0x00000000 here would mean the write never reached the register at all -
  // the command would have "succeeded" by reading back a clear CMDACT that
  // was never set, and everything after it would be fiction.  0x00000009
  // means it really ran.
  //
  DEBUG ((
    DEBUG_ERROR,
    "KirinUsbDevice: DEPSTARTCFG reg 0x%08x\n",
    Reg
    ));

  return SetTransferResources ();
}

/**
  DEPSETEPCONFIG for one endpoint.
**/
EFI_STATUS
SetEpConfig (
  IN UINT8   EpNum,
  IN UINT32  EpType,      // 0 control, 1 isoc, 2 bulk, 3 interrupt
  IN UINT32  MaxPacket,
  IN BOOLEAN IsIn,
  IN BOOLEAN Modify
  )
{
  UINT32  Param0;
  UINT32  Param1;

  Param0 = DWC3_DEPCFG_EP_TYPE (EpType)
            | DWC3_DEPCFG_MAX_PACKET_SIZE (MaxPacket)
            | (Modify ? DWC3_DEPCFG_ACTION_MODIFY : DWC3_DEPCFG_ACTION_INIT);

  if (IsIn) {
    Param0 |= DWC3_DEPCFG_FIFO_NUMBER (EpNum >> 1);
  }

  Param1 = DWC3_DEPCFG_EP_NUMBER (EpNum);

  if (EpType == 0) {
    Param1 |= DWC3_DEPCFG_XFER_COMPLETE_EN;
  }
  if ((EpNum <= 1) || (EpType == 1)) {
    Param1 |= DWC3_DEPCFG_XFER_NOT_READY_EN;
  }
  if (EpType != 0) {
    Param1 |= DWC3_DEPCFG_XFER_IN_PROGRESS_EN;
  }

  //
  // Bulk: XFER_IN_PROGRESS_EN only, exactly as dwc3_gadget_set_ep_config()
  // (gadget.c:599-603) programs it - XFER_COMPLETE_EN is control-only and
  // XFER_NOT_READY_EN is EP0/isoc-only.
  //
  // An earlier revision enabled XFER_COMPLETE_EN here as well "for
  // diagnostics".  That is not merely a deviation from the kernel, it is
  // unsafe: with both bits set the core reports the same finished transfer
  // twice (ev 0x1 then ev 0x2), and both of them fall through to
  // HandleBulkOutComplete()/the bulk IN completion, so one packet would be
  // handed to the transport twice and the residual read from a TRB that has
  // already been recycled.  The kernel leaves bit 8 clear precisely so that
  // bulk completes exactly once, as XFERINPROGRESS.
  //
  // Keeping bulk on XFERINPROGRESS is also what makes the resource model
  // above work: resource_index is only cleared by XFERCOMPLETE, so on bulk it
  // deliberately survives every completion and the next arm is an
  // UPDATETRANSFER rather than a STARTTRANSFER.
  //

  return EpCmd (EpNum, DWC3_DEPCMD_SETEPCONFIG, Param0, Param1, 0, NULL);
}

/**
  Full endpoint enable sequence: configuration round (only when enabling
  EP0-OUT, which always comes first) plus DEPSETEPCONFIG and DALEPENA.
**/
EFI_STATUS
EnableEndpoint (
  IN UINT8   EpNum,
  IN UINT32  EpType,
  IN UINT32  MaxPacket,
  IN BOOLEAN IsIn
  )
{
  EFI_STATUS Status;
  UINT32     Dalep;

  //
  // dwc3_gadget_ep_enable() refuses to touch an endpoint that is already
  // enabled - it warns and returns 0 without issuing a single DEPCMD
  // (gadget.c:797-803).  Without this guard a second SET_CONFIGURATION issues
  // DEPSETEPCONFIG ACTION_INIT on an endpoint that is still enabled, which is
  // the one command the databook forbids: CMDACT never clears, the endpoint's
  // command engine is dead, and every later bulk arm fails instantly with
  // "bulk IN arm failed" before the host has sent a single command.  That is
  // the "booted into fastboot and it is already broken" failure.
  //
  if ((DwRead (DWC3_DALEPENA) & (1u << EpNum)) != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: ep%u already enabled, skip re-enable (dalepena 0x%08x)\n",
      EpNum,
      DwRead (DWC3_DALEPENA)
      ));
    return EFI_SUCCESS;
  }

  if (EpNum == EP0_OUT) {
    Status = StartConfig ();
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = SetEpConfig (EpNum, EpType, MaxPacket, IsIn, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Dalep = DwRead (DWC3_DALEPENA);
  Dalep |= (1u << EpNum);
  DwWrite (DWC3_DALEPENA, Dalep);

  //
  // __dwc3_gadget_ep_enable() does the TRB-ring reset after the DALEPENA
  // write (gadget.c:680-706) and only for non-control endpoints.  Doing it
  // here as well means every SET_CONFIGURATION and every bus reset restarts
  // the bulk rings at slot 0 with the cursors cleared, and - because
  // EnableEndpoint() is the only thing that re-arms a bulk endpoint - the
  // next arm after a reset naturally takes the STARTTRANSFER branch instead
  // of trying to UPDATE a resource the core dropped during the reset.
  //
  if (EpType == 2) {
    if (EpNum == EP_BULK_OUT) {
      InitTrbRing (mBulkOutTrb, &mBulkOutTrbEnq, &mBulkOutTrbDeq);
      mBulkOutResource  = 0;
      mBulkOutBusy      = FALSE;
      mBulkOutRemaining = 0;
    } else if (EpNum == EP_BULK_IN) {
      InitTrbRing (mBulkInTrb, &mBulkInTrbEnq, &mBulkInTrbDeq);
      mBulkInResource = 0;
      mBulkInBusy     = FALSE;
    }
  }

  return EFI_SUCCESS;
}
