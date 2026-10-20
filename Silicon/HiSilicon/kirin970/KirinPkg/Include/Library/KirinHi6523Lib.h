/** @file

  Hi6523 charger IC access over the DesignWare i2c6 master.

  Shared by KirinDwc3Dxe (turns the OTG 5 V boost on when the port is handed
  to the xHCI host at boot) and KirinUsbDeviceDxe (turns it on when the boot
  menu switches the port to host mode, off again before a gadget is pulled
  up).  Both used to carry their own copy of this, and the copy in
  KirinUsbDeviceDxe tested the wrong status bit, so every write failed and
  VBUS was never powered.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef KIRIN_HI6523_LIB_H_
#define KIRIN_HI6523_LIB_H_

#include <Uefi.h>

/**
  Bring i2c6 up and address the Hi6523.

  Ungates the bus clock in the IOMCU CRG, puts gpio198/gpio199 back into I2C
  function mode (the pin controller leaves them in GPIO mode after reset, and
  without this the bus never reaches the chip), pulses the controller reset,
  then programs the DesignWare master.

  Idempotent: cheap enough to call before every transaction.

  @retval RETURN_SUCCESS  i2c6 answered IC_COMP_TYPE and is enabled.
  @return error           The controller is still unreachable.
**/
RETURN_STATUS
EFIAPI
KirinHi6523Init (
  VOID
  );

/**
  Read one Hi6523 register.

  @retval RETURN_SUCCESS  @a Value holds the register.
  @return error           NACK or timeout.
**/
RETURN_STATUS
EFIAPI
KirinHi6523ReadByte (
  IN  UINT8  Reg,
  OUT UINT8  *Value
  );

/**
  Write one Hi6523 register.
**/
RETURN_STATUS
EFIAPI
KirinHi6523WriteByte (
  IN UINT8  Reg,
  IN UINT8  Value
  );

/**
  Read a Hi6523 register, splice @a Value into the field described by
  @a Mask / @a Shift, write it back.  Mirrors the kernel's hi6523_write_mask().
**/
RETURN_STATUS
EFIAPI
KirinHi6523WriteMask (
  IN UINT8  Reg,
  IN UINT8  Mask,
  IN UINT8  Shift,
  IN UINT8  Value
  );

/**
  Give the port its 5 V source so a bus-powered device can be plugged in.

  Stops charging, writes the OTG tuning registers the kernel's
  hi6523_config_otg_opt_param() writes, then sets OTG_REG0.otg_en_int.
  Waits 50 ms for the boost to come up.

  @retval RETURN_SUCCESS  VBUS is being sourced.
  @return error           Nothing was powered; the log says where it stopped.
**/
RETURN_STATUS
EFIAPI
KirinHi6523EnableVbusBoost (
  VOID
  );

/**
  Stop sourcing VBUS, so the phone does not drive 5 V onto a host's own VBUS
  line once it becomes a peripheral.

  @retval RETURN_SUCCESS  Boost is off (or was already).
  @return error           The OTG_EN bit could not be cleared.
**/
RETURN_STATUS
EFIAPI
KirinHi6523DisableVbusBoost (
  VOID
  );

#endif
