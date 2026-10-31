#ifndef DUALSENSE_HID_H
#define DUALSENSE_HID_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DUALSENSE_USB_INPUT_REPORT_SIZE  64
#define DUALSENSE_USB_OUTPUT_REPORT_SIZE 64

#define DUALSENSE_REPORT_ID_INPUT        0x01
#define DUALSENSE_REPORT_ID_OUTPUT       0x02

// DualSense Adaptive Trigger Modes
#define DS_TRIGGER_MODE_OFF              0x00
#define DS_TRIGGER_MODE_FEEDBACK         0x01 // Continuous resistance
#define DS_TRIGGER_MODE_WEAPON           0x02 // Section resistance
#define DS_TRIGGER_MODE_VIBRATION        0x06 // Trigger vibration
#define DS_TRIGGER_MODE_MACHINE          0x26 // Advanced machine

/**
 * Start native 1000 Hz HID input reader thread and output control for DualSense.
 * @param fd USB device file descriptor from Android UsbDeviceConnection.
 * @param controllerNumber Moonlight controller index (0..3).
 * @return 0 on success, negative error code on failure.
 */
int DualSenseHid_Start(int fd, int controllerNumber);

/**
 * Stop native HID reader thread and release USB resources.
 */
void DualSenseHid_Stop(void);

/**
 * Check if the native HID reader is currently active.
 */
bool DualSenseHid_IsRunning(void);

/**
 * Get active controller number (-1 if inactive).
 */
int DualSenseHid_GetControllerNumber(void);

/**
 * Send raw 64-byte Output Report 0x02 to DualSense.
 */
int DualSenseHid_SendOutputReport(const uint8_t* report, int length);

/**
 * Set traditional rumble motor emulation (0..255).
 */
void DualSenseHid_SetRumble(uint8_t leftMotor, uint8_t rightMotor);

/**
 * Set trigger rumble from Moonlight trigger rumble values (0..65535).
 */
void DualSenseHid_SetTriggerRumble(uint16_t leftTrigger, uint16_t rightTrigger);

/**
 * Configure adaptive trigger profile.
 */
void DualSenseHid_SetTriggerEffect(uint8_t triggerIndex, uint8_t mode, const uint8_t* params, int paramLen);

/**
 * Set DualSense adaptive triggers from Sunshine 0x5503 packet.
 */
void DualSenseHid_SetAdaptiveTriggers(uint8_t eventFlags, uint8_t typeLeft, uint8_t typeRight,
                                      const uint8_t* left, const uint8_t* right);

/**
 * Set lightbar RGB LED color.
 */
void DualSenseHid_SetRgbLed(uint8_t r, uint8_t g, uint8_t b);

/**
 * Configure host-requested motion sensor report rate (0 = disabled).
 */
void DualSenseHid_SetMotionEventState(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz);

#ifdef __cplusplus
}
#endif

#endif // DUALSENSE_HID_H
