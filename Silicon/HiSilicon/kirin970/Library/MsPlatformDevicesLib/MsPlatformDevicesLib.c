/** @file
 *MsPlatformDevicesLib  - Device specific library.

Copyright (C) Microsoft Corporation. All rights reserved.
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/UefiLib.h>

VOID
PlatformUpdateAcpiTables(VOID)
{
}

VOID
EFIAPI
PlatformSetup()
{
  PlatformUpdateAcpiTables();
}
