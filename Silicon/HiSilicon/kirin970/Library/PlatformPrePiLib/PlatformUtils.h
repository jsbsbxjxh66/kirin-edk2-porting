#ifndef _PLATFORM_UTILS_H_
#define _PLATFORM_UTILS_H_

#include <Library/PcdLib.h>

// The Kirin970 AP watchdog0 (ARM SP805) register map and kick/enable/disable
// sequences live in <Library/KirinWdt.h> / KirinWdtLib so that every user of
// the watchdog shares one copy.  PlatformInitialize() only turns it off: that
// is the only code that runs before KirinWdtDxe dispatches and starts feeding.
VOID PlatformInitialize();

#endif /* _PLATFORM_UTILS_H_ */
