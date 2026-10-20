/** @file

  DesignWare i2c6 + Hi6523 charger IC, shared by the DWC3 host and device
  drivers.

  Register addresses and sequences are taken from the Kirin 970 kernel:
    drivers/hisi/ap/platform/kirin970/schargerV300_interface.h
    drivers/power/hisi/charger/schargerV300/hisi_scharger_v300.c
    drivers/i2c/busses/i2c-designware-core.c + hisi_i2c_designware.c
    and /amba/i2c@FFD6A000 in the running FDT (pinmux/reset/clock).

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/KirinHi6523Lib.h>

#define I2C6_BASE     0xFFD6A000ULL
#define IOMCU_CRG     0xFFD7E000ULL
#define PINMUX_BASE   0xFFF11000ULL
#define PINCONF_BASE  0xFFF11800ULL

#define IOMCU_RSTEN    0x20
#define IOMCU_CLKEN    0x10
#define IOMCU_RSTDIS   0x24
#define IOMCU_RSTSTAT  0x28
#define I2C6_RST_BIT   27
#define I2C6_CLK_BIT   27

#define DW_IC_CON            0x00
#define DW_IC_TAR            0x04
#define DW_IC_DATA_CMD       0x10
#define DW_IC_SS_SCL_HCNT    0x14
#define DW_IC_SS_SCL_LCNT    0x18
#define DW_IC_FS_SCL_HCNT    0x1C
#define DW_IC_FS_SCL_LCNT    0x20
#define DW_IC_INTR_MASK      0x30
#define DW_IC_RAW_INTR_STAT  0x34
#define DW_IC_CLR_INTR       0x40
#define DW_IC_ENABLE         0x6C
#define DW_IC_STATUS         0x70
#define DW_IC_TX_ABRT_SRC    0x80

#define IC_CON_MASTER      BIT0
#define IC_CON_SPEED_FS    (2u << 1)
#define IC_CON_RESTART_EN  BIT5
#define IC_CON_SLAVE_DIS   BIT6

//
// i2c-designware-core.c:99 defines DW_IC_STATUS_ACTIVITY as 0x1, so bit 0 is
// ACTIVITY and TFE is bit 2.  BIT0 would make DwI2cWaitIdle() wait for "the
// bus went quiet", which never happens while a transfer is in flight, so the
// loop only ever exits on timeout and every write fails.
//
#define IC_STATUS_TFNF  BIT1
#define IC_STATUS_TFE   BIT2
#define IC_STATUS_RFNE  BIT3
#define IC_STATUS_MST   BIT5

#define IC_DATA_CMD_READ  BIT8
#define IC_DATA_CMD_STOP  BIT9

#define HI6523_I2C_ADDR    0x6B
#define HI6523_OTG_REG0    0x80
#define HI6523_OTG_EN_BIT  BIT6
#define HI6523_CHG_REG0    0x90
#define HI6523_CHG_EN_BIT  BIT5

STATIC BOOLEAN  mInited = FALSE;

STATIC
BOOLEAN
TxNotFull (
  VOID
  )
{
  UINTN  Timeout = 5000;

  while (Timeout--) {
    if (MmioRead32 (I2C6_BASE + DW_IC_STATUS) & IC_STATUS_TFNF) {
      return TRUE;
    }

    CpuPause ();
  }

  return FALSE;
}

STATIC
BOOLEAN
WaitIdle (
  VOID
  )
{
  UINTN  Timeout = 50000;

  while (Timeout--) {
    UINT32  St = MmioRead32 (I2C6_BASE + DW_IC_STATUS);

    if ((St & IC_STATUS_TFE) && !(St & IC_STATUS_MST)) {
      return TRUE;
    }

    CpuPause ();
  }

  return FALSE;
}

//
// Bounded wait for the IOMCU reset status bit of i2c6 to reach @a Set.
// Returns FALSE after ~1 ms instead of spinning forever, so a wrong polarity
// or a stuck reset can never hang the boot.
//
STATIC
BOOLEAN
RstWait (
  IN BOOLEAN  Set
  )
{
  UINTN  Retry;

  for (Retry = 0; Retry < 1000; Retry++) {
    BOOLEAN  Now = (MmioRead32 (IOMCU_CRG + IOMCU_RSTSTAT) & (1u << I2C6_RST_BIT)) != 0;

    if (Now == Set) {
      return TRUE;
    }

    MicroSecondDelay (1);
  }

  return FALSE;
}

RETURN_STATUS
EFIAPI
KirinHi6523Init (
  VOID
  )
{
  UINT32  Comps;

  if (mInited) {
    return RETURN_SUCCESS;
  }

  //
  // pinctrl-0 of /amba/i2c@FFD6A000: phandles 0x50/0x51 are gpio199_pmx_func
  // and gpio198_pmx_func (pinmux +0x54 / +0x50 = 0x04), 0x52/0x53 their
  // *_cfg_func (pinconf +0x80 / +0x7c = 0x00).  The pin controller parks both
  // pads in GPIO mode after reset, so without these four stores the bus never
  // reaches the chip.
  //
  MmioWrite32 (PINMUX_BASE + 0x50, 0x04); // gpio198 -> i2c6_sda
  MmioWrite32 (PINMUX_BASE + 0x54, 0x04); // gpio199 -> i2c6_scl
  MmioWrite32 (PINCONF_BASE + 0x7C, 0x00);
  MmioWrite32 (PINCONF_BASE + 0x80, 0x00);

  MmioWrite32 (IOMCU_CRG + IOMCU_CLKEN, 1u << I2C6_CLK_BIT);
  MicroSecondDelay (100);

  //
  // hs_i2c_dw_reset_controller() (hisi_i2c_designware.c:142): assert, wait for
  // RSTSTAT to SET, deassert, wait for it to CLEAR - both bounded.  Waiting
  // the other way round never returns and hangs the boot right after the
  // GSNPSID line, because after assert the status bit IS set.
  //
  MmioWrite32 (IOMCU_CRG + IOMCU_RSTEN, 1u << I2C6_RST_BIT);
  if (!RstWait (TRUE)) {
    DEBUG ((DEBUG_ERROR, "KirinHi6523: i2c6 reset never asserted (RSTSTAT 0x%08x)\n",
            MmioRead32 (IOMCU_CRG + IOMCU_RSTSTAT)));
  }

  MmioWrite32 (IOMCU_CRG + IOMCU_RSTDIS, 1u << I2C6_RST_BIT);
  if (!RstWait (FALSE)) {
    DEBUG ((DEBUG_ERROR, "KirinHi6523: i2c6 reset never cleared (RSTSTAT 0x%08x)\n",
            MmioRead32 (IOMCU_CRG + IOMCU_RSTSTAT)));
  }

  MmioWrite32 (I2C6_BASE + DW_IC_ENABLE, 0);

  Comps = MmioRead32 (I2C6_BASE + 0xFC);
  if (Comps != 0x44570140) {
    DEBUG ((DEBUG_ERROR, "KirinHi6523: i2c6 dead (IC_COMP_TYPE 0x%08x)\n", Comps));
    return RETURN_DEVICE_ERROR;
  }

  //
  // 60/70 gives ~228 kHz rather than the kernel's 27/29 (33.6 MHz / 4 = 8.4
  // MHz source) - slower than asked for, but inside what any part tolerates.
  //
  MmioWrite32 (I2C6_BASE + DW_IC_CON, IC_CON_MASTER | IC_CON_SPEED_FS | IC_CON_RESTART_EN | IC_CON_SLAVE_DIS);
  MmioWrite32 (I2C6_BASE + DW_IC_FS_SCL_HCNT, 60);
  MmioWrite32 (I2C6_BASE + DW_IC_FS_SCL_LCNT, 70);
  MmioWrite32 (I2C6_BASE + DW_IC_TAR, HI6523_I2C_ADDR);
  MmioWrite32 (I2C6_BASE + DW_IC_INTR_MASK, 0);
  MmioRead32 (I2C6_BASE + DW_IC_CLR_INTR);
  MmioWrite32 (I2C6_BASE + DW_IC_ENABLE, 1);

  if (!(MmioRead32 (I2C6_BASE + DW_IC_STATUS) & IC_STATUS_TFNF)) {
    DEBUG ((DEBUG_ERROR, "KirinHi6523: i2c6 not ready after enable\n"));
    return RETURN_DEVICE_ERROR;
  }

  mInited = TRUE;
  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
KirinHi6523WriteByte (
  IN UINT8  Reg,
  IN UINT8  Value
  )
{
  if (!TxNotFull ()) {
    return RETURN_TIMEOUT;
  }

  MmioWrite32 (I2C6_BASE + DW_IC_DATA_CMD, (UINT32)Reg);

  if (!TxNotFull ()) {
    return RETURN_TIMEOUT;
  }

  MmioWrite32 (I2C6_BASE + DW_IC_DATA_CMD, (UINT32)Value | IC_DATA_CMD_STOP);

  if (!WaitIdle ()) {
    return RETURN_TIMEOUT;
  }

  if (MmioRead32 (I2C6_BASE + DW_IC_RAW_INTR_STAT) & BIT6) {
    MmioRead32 (I2C6_BASE + DW_IC_CLR_INTR);
    return RETURN_DEVICE_ERROR;
  }

  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
KirinHi6523ReadByte (
  IN  UINT8  Reg,
  OUT UINT8  *Value
  )
{
  if (Value == NULL) {
    return RETURN_INVALID_PARAMETER;
  }

  *Value = 0;

  if (!TxNotFull ()) {
    return RETURN_TIMEOUT;
  }

  MmioWrite32 (I2C6_BASE + DW_IC_DATA_CMD, (UINT32)Reg);

  //
  // The Hi6523 does not NACK a register it does not know; the kernel reads
  // two bytes and keeps the second.
  //
  if (!TxNotFull ()) {
    return RETURN_TIMEOUT;
  }

  MmioWrite32 (I2C6_BASE + DW_IC_DATA_CMD, IC_DATA_CMD_READ | IC_DATA_CMD_STOP);

  {
    UINTN  Timeout = 50000;

    while (Timeout--) {
      if (MmioRead32 (I2C6_BASE + DW_IC_STATUS) & IC_STATUS_RFNE) {
        *Value = (UINT8)MmioRead32 (I2C6_BASE + DW_IC_DATA_CMD);
        return RETURN_SUCCESS;
      }

      CpuPause ();
    }
  }

  return RETURN_TIMEOUT;
}

RETURN_STATUS
EFIAPI
KirinHi6523WriteMask (
  IN UINT8  Reg,
  IN UINT8  Mask,
  IN UINT8  Shift,
  IN UINT8  Value
  )
{
  EFI_STATUS    Status;
  RETURN_STATUS Ret;
  UINT8         Orig;

  Status = KirinHi6523ReadByte (Reg, &Orig);
  if (EFI_ERROR (Status)) {
    return RETURN_DEVICE_ERROR;
  }

  Orig = (UINT8)((Orig & ~(UINT8)(Mask << Shift)) | (UINT8)((Value << Shift) & Mask));

  Ret = KirinHi6523WriteByte (Reg, Orig);
  if (RETURN_ERROR (Ret)) {
    DEBUG ((DEBUG_ERROR, "KirinHi6523: write 0x%02x -> 0x%02x failed: %r\n", Reg, Orig, Ret));
  }

  return Ret;
}

RETURN_STATUS
EFIAPI
KirinHi6523EnableVbusBoost (
  VOID
  )
{
  RETURN_STATUS  Status;
  UINT8          Reg0;

  Status = KirinHi6523Init ();
  if (RETURN_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "KirinHi6523: VBUS boost off, i2c6 init failed: %r\n", Status));
    return Status;
  }

  //
  // Stop taking power from VBUS before we start pushing it out.
  //
  Status = KirinHi6523WriteMask (HI6523_CHG_REG0, HI6523_CHG_EN_BIT, 5, 0);
  if (RETURN_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "KirinHi6523: VBUS boost off, could not stop charging: %r\n", Status));
    return Status;
  }

  //
  // hi6523_config_otg_opt_param() with otg_lim_set == 3 (the boot-time value,
  // set by scargerV300 init for a 1000 mA limit).  Without these the boost
  // either will not start or comes up at the wrong current limit.
  //
  KirinHi6523WriteByte (0x6a, 0x3f);
  KirinHi6523WriteByte (0x6b, 0x5c);
  KirinHi6523WriteByte (0x81, 0x5c);
  KirinHi6523WriteByte (0x86, 0x39);
  KirinHi6523WriteByte (0x8a, 0x18);
  KirinHi6523WriteByte (0x88, 0x9f);

  Status = KirinHi6523WriteMask (HI6523_OTG_REG0, HI6523_OTG_EN_BIT, 6, 1);
  if (RETURN_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "KirinHi6523: VBUS boost off, could not set OTG_EN: %r\n", Status));
    return Status;
  }

  MicroSecondDelay (50000);

  if (RETURN_ERROR (KirinHi6523ReadByte (HI6523_OTG_REG0, &Reg0))) {
    Reg0 = 0;
  }

  if ((Reg0 & HI6523_OTG_EN_BIT) == 0) {
    DEBUG ((DEBUG_ERROR, "KirinHi6523: VBUS boost did not stick (OTG_REG0 0x%02x)\n", Reg0));
    return RETURN_DEVICE_ERROR;
  }

  DEBUG ((DEBUG_ERROR, "KirinHi6523: VBUS 5V boost on (OTG_REG0 0x%02x)\n", Reg0));
  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
KirinHi6523DisableVbusBoost (
  VOID
  )
{
  RETURN_STATUS  Status;
  UINT8          Orig;

  Status = KirinHi6523Init ();
  if (RETURN_ERROR (Status)) {
    return Status;
  }

  Status = KirinHi6523ReadByte (HI6523_OTG_REG0, &Orig);
  if (RETURN_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "KirinHi6523: read OTG_REG0 failed: %r\n", Status));
    return Status;
  }

  if ((Orig & HI6523_OTG_EN_BIT) == 0) {
    return RETURN_SUCCESS;
  }

  Status = KirinHi6523WriteByte (HI6523_OTG_REG0, (UINT8)(Orig & ~HI6523_OTG_EN_BIT));
  if (RETURN_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "KirinHi6523: could not clear OTG_EN: %r\n", Status));
    return Status;
  }

  MicroSecondDelay (20000);
  DEBUG ((DEBUG_ERROR, "KirinHi6523: VBUS 5V boost off (OTG_REG0 was 0x%02x)\n", Orig));
  return RETURN_SUCCESS;
}
