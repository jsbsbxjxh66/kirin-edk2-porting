/** @file

  Minimal SPMI access to the Kirin970 PMIC (hi6421v600).

  See Library/KirinPmicSpmiLib.h for the hardware reference.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/KirinPmicSpmiLib.h>

//
// Same numbers as drivers/spmi_hisi/hisi-spmi-controller.c and the DTS node
// /spmi@FFF24000 { spmi-channel = <2>; } + /spmi@FFF24000/pmic@0 { slave_id = 0 }.
//
#define SPMI_BASE             0xFFF24000
#define SPMI_CHANNEL          2
#define SPMI_SID              0

#define SPMI_CHANNEL_OFFSET   0x300
#define SPMI_SLAVE_OFFSET     0x20
#define SPMI_CMD_OFFSET       0x0100
#define SPMI_WDATA0_OFFSET    0x0104
#define SPMI_STATUS_OFFSET    0x0200
#define SPMI_RDATA0_OFFSET    0x0204

#define SPMI_CMD_EN           (1U << 31)
#define SPMI_CMD_TYPE_SHIFT   24
#define SPMI_CMD_LENGTH_SHIFT 20
#define SPMI_CMD_SID_SHIFT    16
#define SPMI_CMD_EXT_WRITE_L  5
#define SPMI_CMD_EXT_READ_L   6

#define SPMI_TRANS_DONE       (1U << 0)
#define SPMI_TRANS_FAIL       (1U << 2)
#define SPMI_TIMEOUT_US       1000

//
// Command base, offset by channel.  The status register additionally carries
// the per-slave offset; the data register does too, which is what puts the
// payload in a different word from the command.
//
STATIC
UINTN
SpmiChannelBase (
  VOID
  )
{
  return SPMI_BASE + SPMI_CHANNEL_OFFSET * SPMI_CHANNEL;
}

STATIC
RETURN_STATUS
SpmiWaitForDone (
  VOID
  )
{
  UINTN  StatusReg = SpmiChannelBase () +
                     SPMI_STATUS_OFFSET +
                     SPMI_SLAVE_OFFSET * SPMI_SID;
  UINTN  Index;

  for (Index = 0; Index < SPMI_TIMEOUT_US; Index++) {
    UINT32 Status = MmioRead32 (StatusReg);

    if (Status & SPMI_TRANS_DONE) {
      return (Status & SPMI_TRANS_FAIL) ? RETURN_DEVICE_ERROR : RETURN_SUCCESS;
    }

    MicroSecondDelay (1);
  }

  return RETURN_TIMEOUT;
}

RETURN_STATUS
EFIAPI
KirinPmicRead8 (
  IN  UINT16  Reg,
  OUT UINT8   *Value
  )
{
  UINTN  Base;
  UINT32 Cmd;

  if (Value == NULL) {
    return RETURN_INVALID_PARAMETER;
  }

  *Value = 0;
  Base   = SpmiChannelBase ();

  Cmd = SPMI_CMD_EN |
        (SPMI_CMD_EXT_READ_L << SPMI_CMD_TYPE_SHIFT) |
        ((1 - 1) << SPMI_CMD_LENGTH_SHIFT) |
        ((SPMI_SID & 0xF) << SPMI_CMD_SID_SHIFT) |
        (Reg & 0xFFFF);

  MmioWrite32 (Base + SPMI_CMD_OFFSET, Cmd);

  if (RETURN_ERROR (SpmiWaitForDone ())) {
    return RETURN_TIMEOUT;
  }

  // The controller driver byte-swaps the 32-bit data register (Tranverse32)
  // and hands back its low byte, so the payload sits in bits 31:24 as seen
  // from here.
  *Value = (UINT8)(MmioRead32 (
                     Base +
                     SPMI_SLAVE_OFFSET * SPMI_SID +
                     SPMI_RDATA0_OFFSET) >> 24);

  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
KirinPmicWrite8 (
  IN  UINT16  Reg,
  IN  UINT8   Value
  )
{
  UINTN  Base;
  UINT32 Cmd;

  Base = SpmiChannelBase ();

  Cmd = SPMI_CMD_EN |
        (SPMI_CMD_EXT_WRITE_L << SPMI_CMD_TYPE_SHIFT) |
        ((1 - 1) << SPMI_CMD_LENGTH_SHIFT) |
        ((SPMI_SID & 0xF) << SPMI_CMD_SID_SHIFT) |
        (Reg & 0xFFFF);

  // Same byte order as the read path: the payload goes into bits 31:24,
  // which is where the controller's byte swap puts it.
  MmioWrite32 (Base + SPMI_WDATA0_OFFSET, ((UINT32)Value) << 24);
  MmioWrite32 (Base + SPMI_CMD_OFFSET, Cmd);

  return SpmiWaitForDone ();
}

INT32
EFIAPI
KirinPmicGetVbusStatus (
  VOID
  )
{
  UINT8  Value;

  if (RETURN_ERROR (KirinPmicRead8 (KIRIN_PMIC_VBUS_REG, &Value))) {
    return -1;
  }

  return ((Value & (1U << KIRIN_PMIC_VBUS_BIT)) != 0) ? 1 : 0;
}
