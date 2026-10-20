/** @file

  Minimal SPMI access to the Kirin970 PMIC (hi6421v600).

  The PMIC sits behind the HiSilicon SPMI controller at 0xFFF24000 and is
  reached with a single EXT_READ_L / EXT_WRITE_L command; both are plain MMIO
  plus a bounded poll, so this library is safe to call from any TPL.

    Device tree:
      /spmi@FFF24000                    hisilicon,spmi-controller
                                        spmi-channel = <0x02>, reg 0x1000
      /spmi@FFF24000/pmic@0             slave_id = <0x00>
        hisilicon,hisi-pmic-vbus = <0x6 1>   (VBUS status register + bit)

    Register layout and transaction sequence come from
    drivers/spmi_hisi/hisi-spmi-controller.c (spmi_read_cmd / spmi_write_cmd)
    and drivers/mfd/hisi_pmic_spmi.c (hisi_pmic_reg_read /
    hisi_pmic_get_vbus_status).

  There are two other copies of this sequence in the tree
  (KirinGpioKeyDxe, KirinRebootModeDxe); both were folded into this library
  so a byte-order or timeout fix only has to land once.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/
#ifndef __KIRIN_PMIC_SPMI_LIB_H__
#define __KIRIN_PMIC_SPMI_LIB_H__

#include <Uefi.h>

//
// hisilicon,hisi-pmic-vbus = <0x6 1>: VBUS-presence status lives in PMIC
// register 0x6, bit 1.  Read by the kernel's hisi_pmic_get_vbus_status() and
// reported straight through dwc3-hisi.c's hisi_usb_vbus_value().
//
#define KIRIN_PMIC_VBUS_REG  0x6
#define KIRIN_PMIC_VBUS_BIT  1

/**
  Read one PMIC register over SPMI.

  @param[in]   Reg    16-bit PMIC register address.
  @param[out]  Value  The byte read, or 0 if the transfer failed.

  @retval RETURN_SUCCESS  The register was read.
  @retval RETURN_TIMEOUT  The controller did not report TRANS_DONE in
                          SPMI_TIMEOUT_US.
**/
RETURN_STATUS
EFIAPI
KirinPmicRead8 (
  IN  UINT16  Reg,
  OUT UINT8   *Value
  );

/**
  Write one PMIC register over SPMI.

  @param[in]  Reg    16-bit PMIC register address.
  @param[in]  Value  The byte to write.

  @retval RETURN_SUCCESS  The register was written.
  @retval RETURN_TIMEOUT  The controller did not report TRANS_DONE in
                          SPMI_TIMEOUT_US.
**/
RETURN_STATUS
EFIAPI
KirinPmicWrite8 (
  IN  UINT16  Reg,
  IN  UINT8   Value
  );

/**
  Report whether VBUS is present at the PMIC.

  @retval  1   VBUS is present (a host or charger is feeding the port).
  @retval  0   VBUS is absent.
  @retval -1   The PMIC register could not be read.
**/
INT32
EFIAPI
KirinPmicGetVbusStatus (
  VOID
  );

#endif
