#ifndef _DEVICE_CONFIGURATION_MAP_H_
#define _DEVICE_CONFIGURATION_MAP_H_

#define CONFIGURATION_NAME_MAX_LENGTH 64

typedef struct {
  CHAR8                        Name[CONFIGURATION_NAME_MAX_LENGTH];
  UINT64                       Value;
} CONFIGURATION_DESCRIPTOR_EX, *PCONFIGURATION_DESCRIPTOR_EX;

static CONFIGURATION_DESCRIPTOR_EX gDeviceConfigurationDescriptorEx[] = {
    {"DetectRetailUserAttentionHotkey", 0x00},
    {"DetectRetailUserAttentionHotkeyCode", 0x17},
    {"EarlyInitCoreCnt", 1},
    {"EnableACPIFallback", 0x0},
    {"EnableDisplayImageFv", 0x0},
    {"EnableDisplayThread", 0},
    {"EnableMultiThreading", 0},
    {"EnableOEMSetupAppInRetail", 0x0},
    {"EnablePXE", 0x0},
    {"EnableSDHCSwitch", 0x1},
    {"EnableShell", 0x1},
    {"EnableVariablePolicyEngine", 7},
    {"MaxCoreCnt", 8},
    {"MaxLogFileSize", 0x800000},
    {"NumActiveCores", 8},
    {"NumCpus", 8},
    {"PwrBtnShutdownFlag", 0x0},
    {"SecurityFlag", 0x1C77},
    {"SerialPortBufferSize", 0x8000},
    {"SharedIMEMBaseAddr", 0},
    {"DloadCookieAddr", 0},
    {"DloadCookieValue", 0},
    /* Terminator */
    {"Terminator", 0xFFFFFFFF}};

#endif
