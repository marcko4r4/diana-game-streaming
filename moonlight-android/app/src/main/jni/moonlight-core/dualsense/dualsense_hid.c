#include "dualsense_hid.h"
#include "dualsense_audio.h"

#include <Limelight.h>
#include <Platform.h>

#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <android/log.h>

#include <stdlib.h>

#define TAG "DualSenseNativeHID"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define DUALSENSE_HID_INTERFACE 3
#define DUALSENSE_EP_IN         0x84
#define DUALSENSE_EP_OUT        0x03

typedef struct {
    int fd;
    int controllerNumber;
    volatile bool running;
    volatile bool shouldStop;
    volatile bool arrivalSent;
    pthread_t readerThread;
    pthread_mutex_t outputMutex;
    uint8_t outputReport[DUALSENSE_USB_OUTPUT_REPORT_SIZE];
    uint64_t lastInputSentMs;
    uint64_t lastAccelMs;
    uint64_t lastGyroMs;
    uint64_t lastBatteryMs;
    volatile uint16_t accelReportRateHz;
    volatile uint16_t gyroReportRateHz;
    bool hasSentInitialInput;
    int prevButtonFlags;
    uint8_t prevLt;
    uint8_t prevRt;
    short prevLx;
    short prevLy;
    short prevRx;
    short prevRy;
    int16_t prevGyroX, prevGyroY, prevGyroZ;
    int16_t prevAccelX, prevAccelY, prevAccelZ;
    bool touchActive[2];
    uint16_t touchX[2];
    uint16_t touchY[2];
} DualSenseState;

static DualSenseState g_Device = {
    .fd = -1,
    .controllerNumber = -1,
    .running = false,
    .shouldStop = false,
    .arrivalSent = false,
    .outputMutex = PTHREAD_MUTEX_INITIALIZER,
    .lastInputSentMs = 0,
    .lastAccelMs = 0,
    .lastGyroMs = 0,
    .lastBatteryMs = 0,
    .accelReportRateHz = 0,
    .gyroReportRateHz = 0,
    .hasSentInitialInput = false,
    .prevButtonFlags = 0,
    .prevLt = 0,
    .prevRt = 0,
    .prevLx = 0,
    .prevLy = 0,
    .prevRx = 0,
    .prevRy = 0,
    .prevGyroX = 0, .prevGyroY = 0, .prevGyroZ = 0,
    .prevAccelX = 0, .prevAccelY = 0, .prevAccelZ = 0,
    .touchActive = { false, false },
    .touchX = { 0, 0 },
    .touchY = { 0, 0 }
};

static bool DualSenseHid_EnsureArrivalSent(DualSenseState* dev) {
    if (dev->arrivalSent) {
        return true;
    }

    uint16_t caps = LI_CCAP_ANALOG_TRIGGERS |
                    LI_CCAP_RUMBLE |
                    LI_CCAP_TRIGGER_RUMBLE |
                    LI_CCAP_TOUCHPAD |
                    LI_CCAP_ACCEL |
                    LI_CCAP_GYRO |
                    LI_CCAP_BATTERY_STATE |
                    LI_CCAP_RGB_LED |
                    LI_CCAP_HAPTICS_PCM;

    int err = LiSendControllerArrivalEvent((uint8_t)dev->controllerNumber,
                                           (uint16_t)(1 << dev->controllerNumber),
                                           LI_CTYPE_PS,
                                           0xFFFFFFFFU,
                                           caps);
    if (err == 0) {
        dev->arrivalSent = true;
        LOGI("Sent DualSense ControllerArrivalEvent for controller %d (type=LI_CTYPE_PS, caps=0x%04x)",
             dev->controllerNumber, caps);
        return true;
    }

    return false;
}

static int DualSenseHid_SendOutputReportLocked(void) {
    if (g_Device.fd < 0) {
        return -1;
    }

    // Primary: Send via Interrupt OUT Endpoint 0x03 (USBDEVFS_BULK), matching Linux hid-playstation
    struct usbdevfs_bulktransfer bulk;
    memset(&bulk, 0, sizeof(bulk));
    bulk.ep = DUALSENSE_EP_OUT;
    bulk.len = DUALSENSE_USB_OUTPUT_REPORT_SIZE;
    bulk.timeout = 100;
    bulk.data = g_Device.outputReport;

    int res = ioctl(g_Device.fd, USBDEVFS_BULK, &bulk);
    if (res < 0) {
        // Fallback: Try USBDEVFS_CONTROL (HID SET_REPORT on interface 3)
        struct usbdevfs_ctrltransfer ctrl;
        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.bRequestType = 0x21; // USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE
        ctrl.bRequest = 0x09;     // SET_REPORT
        ctrl.wValue = (0x02 << 8) | DUALSENSE_REPORT_ID_OUTPUT; // (OUTPUT << 8) | 0x02
        ctrl.wIndex = DUALSENSE_HID_INTERFACE;                  // Interface 3
        ctrl.wLength = DUALSENSE_USB_OUTPUT_REPORT_SIZE;
        ctrl.data = g_Device.outputReport;
        ctrl.timeout = 100; // ms
        res = ioctl(g_Device.fd, USBDEVFS_CONTROL, &ctrl);
    }

    return res;
}

static inline short DualSenseHid_ScaleStickAxis(uint8_t raw, bool invert) {
    int centered = (int)raw - 128; // -128..127
    // Filter tiny 2-LSB resting ADC jitter at center (126..130 -> 0)
    if (centered >= -2 && centered <= 2) {
        return 0;
    }
    if (invert) {
        centered = -centered;      // -127..128
    }
    int scaled = (centered >= 0)
        ? (centered * 32767) / (invert ? 128 : 127)
        : (centered * 32767) / (invert ? 127 : 128);
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32767) scaled = -32767;
    return (short)scaled;
}

static void DualSenseHid_ProcessInputReport(DualSenseState* dev, const uint8_t* buf, int len) {
    if (len < 11 || buf[0] != DUALSENSE_REPORT_ID_INPUT) {
        return;
    }

    // Sticks: DualSense reports 0..255 with 128 as center
    // Moonlight expects -32767..32767 with Y inverted (positive = up)
    short lx = DualSenseHid_ScaleStickAxis(buf[1], false);
    short ly = DualSenseHid_ScaleStickAxis(buf[2], true);
    short rx = DualSenseHid_ScaleStickAxis(buf[3], false);
    short ry = DualSenseHid_ScaleStickAxis(buf[4], true);

    // Triggers: 0..255
    uint8_t lt = buf[5];
    uint8_t rt = buf[6];

    // D-Pad Hat Switch (buf[8] lower 4 bits; buf[7] is seq_number!)
    uint8_t dpad = buf[8] & 0x0F;
    int buttonFlags = 0;
    if (dpad == 0 || dpad == 1 || dpad == 7) buttonFlags |= UP_FLAG;
    if (dpad == 1 || dpad == 2 || dpad == 3) buttonFlags |= RIGHT_FLAG;
    if (dpad == 3 || dpad == 4 || dpad == 5) buttonFlags |= DOWN_FLAG;
    if (dpad == 5 || dpad == 6 || dpad == 7) buttonFlags |= LEFT_FLAG;

    // Action buttons (buf[8])
    uint8_t b8 = buf[8];
    if (b8 & 0x10) buttonFlags |= X_FLAG; // Square
    if (b8 & 0x20) buttonFlags |= A_FLAG; // Cross
    if (b8 & 0x40) buttonFlags |= B_FLAG; // Circle
    if (b8 & 0x80) buttonFlags |= Y_FLAG; // Triangle

    // Shoulder & Center buttons (buf[9])
    uint8_t b9 = buf[9];
    if (b9 & 0x01) buttonFlags |= LB_FLAG; // L1
    if (b9 & 0x02) buttonFlags |= RB_FLAG; // R1
    if (b9 & 0x10) buttonFlags |= BACK_FLAG; // Create / Share
    if (b9 & 0x20) buttonFlags |= PLAY_FLAG; // Options
    if (b9 & 0x40) buttonFlags |= LS_CLK_FLAG; // L3
    if (b9 & 0x80) buttonFlags |= RS_CLK_FLAG; // R3

    // System buttons (buf[10])
    uint8_t b10 = buf[10];
    if (b10 & 0x01) buttonFlags |= SPECIAL_FLAG;  // PS Home
    if (b10 & 0x02) buttonFlags |= TOUCHPAD_FLAG; // Touchpad Click
    if (b10 & 0x04) buttonFlags |= MISC_FLAG;     // Mic Mute

    // Ensure ControllerArrivalEvent is queued before any MultiController input packet
    if (!DualSenseHid_EnsureArrivalSent(dev)) {
        return;
    }

    uint64_t now = PltGetMillis();

    // State deduplication & rate-limiting to avoid 1000 Hz reliable ENet packet flooding:
    // - Button changes or return-to-zero on sticks/triggers are sent immediately (0 ms delay)
    // - Analog axis movements are rate-limited to 125 Hz (8 ms) and ignore 1-LSB ADC noise when deflected
    bool buttonsChanged = !dev->hasSentInitialInput || (buttonFlags != dev->prevButtonFlags);
    bool triggersZeroChange = (lt == 0 && dev->prevLt != 0) || (rt == 0 && dev->prevRt != 0) ||
                              (lt == 255 && dev->prevLt != 255) || (rt == 255 && dev->prevRt != 255);
    bool sticksZeroChange = (lx == 0 && dev->prevLx != 0) || (ly == 0 && dev->prevLy != 0) ||
                            (rx == 0 && dev->prevRx != 0) || (ry == 0 && dev->prevRy != 0);
    bool analogChanged = (lt != dev->prevLt) || (rt != dev->prevRt) ||
                         (abs((int)lx - (int)dev->prevLx) > 300) ||
                         (abs((int)ly - (int)dev->prevLy) > 300) ||
                         (abs((int)rx - (int)dev->prevRx) > 300) ||
                         (abs((int)ry - (int)dev->prevRy) > 300);

    if (buttonsChanged || triggersZeroChange || sticksZeroChange ||
        (analogChanged && (now - dev->lastInputSentMs >= 8))) {
        dev->hasSentInitialInput = true;
        dev->lastInputSentMs = now;
        dev->prevButtonFlags = buttonFlags;
        dev->prevLt = lt;
        dev->prevRt = rt;
        dev->prevLx = lx;
        dev->prevLy = ly;
        dev->prevRx = rx;
        dev->prevRy = ry;

        LiSendMultiControllerEvent((short)dev->controllerNumber, (short)(1 << dev->controllerNumber),
                                   buttonFlags, lt, rt, lx, ly, rx, ry);
    }

    // Motion Sensors (Gyro & Accel) at offset 16 — only send when requested by host via SetMotionEventState
    if (len >= 28) {
        int16_t rawGyroX = (int16_t)(buf[16] | (buf[17] << 8));
        int16_t rawGyroY = (int16_t)(buf[18] | (buf[19] << 8));
        int16_t rawGyroZ = (int16_t)(buf[20] | (buf[21] << 8));
        int16_t rawAccelX = (int16_t)(buf[22] | (buf[23] << 8));
        int16_t rawAccelY = (int16_t)(buf[24] | (buf[25] << 8));
        int16_t rawAccelZ = (int16_t)(buf[26] | (buf[27] << 8));

        uint16_t accelRate = dev->accelReportRateHz;
        if (accelRate > 0) {
            uint64_t intervalMs = (accelRate >= 200) ? 5 : (1000 / accelRate);
            if (now - dev->lastAccelMs >= intervalMs) {
                if (rawAccelX != dev->prevAccelX || rawAccelY != dev->prevAccelY || rawAccelZ != dev->prevAccelZ) {
                    dev->lastAccelMs = now;
                    dev->prevAccelX = rawAccelX;
                    dev->prevAccelY = rawAccelY;
                    dev->prevAccelZ = rawAccelZ;
                    float accelX = ((float)rawAccelX / 8192.0f) * 9.80665f;
                    float accelY = ((float)rawAccelY / 8192.0f) * 9.80665f;
                    float accelZ = ((float)rawAccelZ / 8192.0f) * 9.80665f;
                    LiSendControllerMotionEvent((uint8_t)dev->controllerNumber, LI_MOTION_TYPE_ACCEL, accelX, accelY, accelZ);
                }
            }
        }

        uint16_t gyroRate = dev->gyroReportRateHz;
        if (gyroRate > 0) {
            uint64_t intervalMs = (gyroRate >= 200) ? 5 : (1000 / gyroRate);
            if (now - dev->lastGyroMs >= intervalMs) {
                if (rawGyroX != dev->prevGyroX || rawGyroY != dev->prevGyroY || rawGyroZ != dev->prevGyroZ) {
                    dev->lastGyroMs = now;
                    dev->prevGyroX = rawGyroX;
                    dev->prevGyroY = rawGyroY;
                    dev->prevGyroZ = rawGyroZ;
                    float gyroX = (float)rawGyroX / 16.0f;
                    float gyroY = (float)rawGyroY / 16.0f;
                    float gyroZ = (float)rawGyroZ / 16.0f;
                    LiSendControllerMotionEvent((uint8_t)dev->controllerNumber, LI_MOTION_TYPE_GYRO, gyroX, gyroY, gyroZ);
                }
            }
        }
    }

    // Touchpad 2-point capacitive touch at offset 33..40 (1920x1080 resolution)
    if (len >= 41) {
        for (int i = 0; i < 2; i++) {
            int offset = 33 + (i * 4);
            bool active = (buf[offset] & 0x80) == 0;
            uint16_t rawX = (uint16_t)(buf[offset + 1] | ((buf[offset + 2] & 0x0F) << 8));
            uint16_t rawY = (uint16_t)((buf[offset + 2] >> 4) | (buf[offset + 3] << 4));
            float normX = (float)rawX / 1920.0f;
            float normY = (float)rawY / 1080.0f;
            if (normX > 1.0f) normX = 1.0f;
            if (normY > 1.0f) normY = 1.0f;

            if (active && !dev->touchActive[i]) {
                dev->touchActive[i] = true;
                dev->touchX[i] = rawX;
                dev->touchY[i] = rawY;
                LiSendControllerTouchEvent((uint8_t)dev->controllerNumber, LI_TOUCH_EVENT_DOWN, (uint32_t)i, normX, normY, 1.0f);
            } else if (active && dev->touchActive[i]) {
                if (rawX != dev->touchX[i] || rawY != dev->touchY[i]) {
                    dev->touchX[i] = rawX;
                    dev->touchY[i] = rawY;
                    LiSendControllerTouchEvent((uint8_t)dev->controllerNumber, LI_TOUCH_EVENT_MOVE, (uint32_t)i, normX, normY, 1.0f);
                }
            } else if (!active && dev->touchActive[i]) {
                dev->touchActive[i] = false;
                LiSendControllerTouchEvent((uint8_t)dev->controllerNumber, LI_TOUCH_EVENT_UP, (uint32_t)i, normX, normY, 0.0f);
            }
        }
    }

    // Battery state at offset 53 (report every 10 seconds)
    if (len >= 54) {
        uint64_t now = PltGetMillis();
        if (dev->lastBatteryMs == 0 || (now - dev->lastBatteryMs >= 10000)) {
            dev->lastBatteryMs = now;
            uint8_t batLevel = buf[53] & 0x0F;
            uint8_t batStatus = (buf[53] >> 4) & 0x0F;
            uint8_t liState = LI_BATTERY_STATE_UNKNOWN;
            if (batStatus == 0x0) liState = LI_BATTERY_STATE_DISCHARGING;
            else if (batStatus == 0x1) liState = LI_BATTERY_STATE_CHARGING;
            else if (batStatus == 0x2) liState = LI_BATTERY_STATE_FULL;
            uint8_t pct = (batLevel >= 10) ? 100 : (uint8_t)(batLevel * 10 + 5);
            if (liState == LI_BATTERY_STATE_FULL) pct = 100;
            LiSendControllerBatteryEvent((uint8_t)dev->controllerNumber, liState, pct);
        }
    }
}

static void* DualSenseHid_ReaderThread(void* arg) {
    DualSenseState* dev = (DualSenseState*)arg;
    uint8_t in_buffer[DUALSENSE_USB_INPUT_REPORT_SIZE];
    struct usbdevfs_bulktransfer bulk;

    LOGI("Native 1000 Hz reader thread started for fd=%d, controller=%d", dev->fd, dev->controllerNumber);

    DualSenseHid_EnsureArrivalSent(dev);

    while (!dev->shouldStop) {
        memset(&bulk, 0, sizeof(bulk));
        bulk.ep = DUALSENSE_EP_IN;
        bulk.len = sizeof(in_buffer);
        bulk.timeout = 500; // 500 ms timeout to allow clean termination check
        bulk.data = in_buffer;

        int res = ioctl(dev->fd, USBDEVFS_BULK, &bulk);
        if (res > 0) {
            DualSenseHid_ProcessInputReport(dev, in_buffer, res);
        } else if (res < 0) {
            if (dev->shouldStop) {
                break;
            }
            if (errno == ETIMEDOUT) {
                continue;
            }
            if (errno == ENODEV || errno == ESHUTDOWN) {
                LOGW("USB device disconnected (errno=%d)", errno);
                break;
            }
            usleep(2000); // 2ms sleep on error
        }
    }

    LOGI("Native 1000 Hz reader thread exited");
    return NULL;
}

int DualSenseHid_Start(int fd, int controllerNumber) {
    if (fd < 0) {
        return -EINVAL;
    }

    if (g_Device.running) {
        DualSenseHid_Stop();
    }

    g_Device.fd = fd;
    g_Device.controllerNumber = controllerNumber;
    g_Device.shouldStop = false;
    g_Device.arrivalSent = false;
    g_Device.lastInputSentMs = 0;
    g_Device.lastAccelMs = 0;
    g_Device.lastGyroMs = 0;
    g_Device.lastBatteryMs = 0;
    g_Device.hasSentInitialInput = false;
    g_Device.prevButtonFlags = 0;
    g_Device.prevLt = 0;
    g_Device.prevRt = 0;
    g_Device.prevLx = 0;
    g_Device.prevLy = 0;
    g_Device.prevRx = 0;
    g_Device.prevRy = 0;
    g_Device.prevGyroX = 0; g_Device.prevGyroY = 0; g_Device.prevGyroZ = 0;
    g_Device.prevAccelX = 0; g_Device.prevAccelY = 0; g_Device.prevAccelZ = 0;
    g_Device.touchActive[0] = false;
    g_Device.touchActive[1] = false;

    // Claim HID interface 3
    int iface = DUALSENSE_HID_INTERFACE;
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0) {
        struct usbdevfs_disconnect_claim dc;
        memset(&dc, 0, sizeof(dc));
        dc.interface = DUALSENSE_HID_INTERFACE;
        dc.flags = USBDEVFS_DISCONNECT_CLAIM_EXCEPT_DRIVER;
        strcpy(dc.driver, "usbfs");
        ioctl(fd, USBDEVFS_DISCONNECT_CLAIM, &dc);
    }

    // Initialize output report (lightbar blue, player 1 indicator)
    pthread_mutex_lock(&g_Device.outputMutex);
    memset(g_Device.outputReport, 0, sizeof(g_Device.outputReport));
    g_Device.outputReport[0] = DUALSENSE_REPORT_ID_OUTPUT;
    g_Device.outputReport[1] = 0x03; // Valid flags 0: Enable rumble & haptics
    g_Device.outputReport[2] = 0x14; // Valid flags 1: Enable LED color & player LEDs
    g_Device.outputReport[39] = 0x02; // Player LED brightness: medium
    g_Device.outputReport[42] = 0x02; // Lightbar setup flag
    g_Device.outputReport[44] = 0x04; // Player 1 LED (center)
    g_Device.outputReport[45] = 0;    // Red
    g_Device.outputReport[46] = 0;    // Green
    g_Device.outputReport[47] = 255;  // Blue
    DualSenseHid_SendOutputReportLocked();
    pthread_mutex_unlock(&g_Device.outputMutex);

    g_Device.running = true;

    if (pthread_create(&g_Device.readerThread, NULL, DualSenseHid_ReaderThread, &g_Device) != 0) {
        LOGE("Failed to create reader thread: %s", strerror(errno));
        g_Device.running = false;
        return -errno;
    }

    LOGI("DualSense native HID engine started successfully");
    // Start native Isochronous audio streaming engine for waveform haptics
    DualSenseAudio_Start(fd, controllerNumber);
    return 0;
}

void DualSenseHid_Stop(void) {
    if (!g_Device.running) {
        return;
    }

    // Stop native Isochronous audio streaming engine
    DualSenseAudio_Stop();

    g_Device.shouldStop = true;
    g_Device.running = false;

    // Reset rumble, triggers and LEDs
    pthread_mutex_lock(&g_Device.outputMutex);
    g_Device.outputReport[3] = 0;
    g_Device.outputReport[4] = 0;
    g_Device.outputReport[11] = 0;
    g_Device.outputReport[22] = 0;
    DualSenseHid_SendOutputReportLocked();
    pthread_mutex_unlock(&g_Device.outputMutex);

    pthread_join(g_Device.readerThread, NULL);

    g_Device.fd = -1;
    g_Device.controllerNumber = -1;
    g_Device.accelReportRateHz = 0;
    g_Device.gyroReportRateHz = 0;

    LOGI("DualSense native HID engine stopped successfully");
}

bool DualSenseHid_IsRunning(void) {
    return g_Device.running;
}

int DualSenseHid_GetControllerNumber(void) {
    return g_Device.controllerNumber;
}

int DualSenseHid_SendOutputReport(const uint8_t* report, int length) {
    if (!g_Device.running || report == NULL || length <= 0) {
        return -1;
    }

    pthread_mutex_lock(&g_Device.outputMutex);
    int copyLen = length < DUALSENSE_USB_OUTPUT_REPORT_SIZE ? length : DUALSENSE_USB_OUTPUT_REPORT_SIZE;
    memcpy(g_Device.outputReport, report, copyLen);
    int ret = DualSenseHid_SendOutputReportLocked();
    pthread_mutex_unlock(&g_Device.outputMutex);

    return ret;
}

void DualSenseHid_SetRumble(uint8_t leftMotor, uint8_t rightMotor) {
    if (!g_Device.running) {
        return;
    }

    pthread_mutex_lock(&g_Device.outputMutex);
    g_Device.outputReport[0] = DUALSENSE_REPORT_ID_OUTPUT;
    g_Device.outputReport[1] |= 0x03; // Valid flags 0: Enable rumble
    g_Device.outputReport[3] = rightMotor; // High freq
    g_Device.outputReport[4] = leftMotor;  // Low freq
    DualSenseHid_SendOutputReportLocked();
    pthread_mutex_unlock(&g_Device.outputMutex);
}

void DualSenseHid_SetTriggerRumble(uint16_t leftTrigger, uint16_t rightTrigger) {
    if (!g_Device.running) {
        return;
    }

    pthread_mutex_lock(&g_Device.outputMutex);
    g_Device.outputReport[0] = DUALSENSE_REPORT_ID_OUTPUT;
    g_Device.outputReport[1] |= 0x0C; // Valid flags 0: Enable left (0x08) & right (0x04) triggers

    // Right Trigger
    if (rightTrigger > 0) {
        uint8_t force = (uint8_t)(((uint32_t)rightTrigger * 7) / 65535 + 1);
        g_Device.outputReport[11] = DS_TRIGGER_MODE_FEEDBACK;
        g_Device.outputReport[12] = 0x00;
        g_Device.outputReport[13] = force;
    } else {
        g_Device.outputReport[11] = DS_TRIGGER_MODE_OFF;
        g_Device.outputReport[12] = 0x00;
        g_Device.outputReport[13] = 0x00;
    }

    // Left Trigger
    if (leftTrigger > 0) {
        uint8_t force = (uint8_t)(((uint32_t)leftTrigger * 7) / 65535 + 1);
        g_Device.outputReport[22] = DS_TRIGGER_MODE_FEEDBACK;
        g_Device.outputReport[23] = 0x00;
        g_Device.outputReport[24] = force;
    } else {
        g_Device.outputReport[22] = DS_TRIGGER_MODE_OFF;
        g_Device.outputReport[23] = 0x00;
        g_Device.outputReport[24] = 0x00;
    }

    DualSenseHid_SendOutputReportLocked();
    pthread_mutex_unlock(&g_Device.outputMutex);
}

void DualSenseHid_SetTriggerEffect(uint8_t triggerIndex, uint8_t mode, const uint8_t* params, int paramLen) {
    if (!g_Device.running) {
        return;
    }

    pthread_mutex_lock(&g_Device.outputMutex);
    g_Device.outputReport[0] = DUALSENSE_REPORT_ID_OUTPUT;

    int offset = (triggerIndex == 0) ? 22 : 11; // 0 = Left trigger, 1 = Right trigger
    g_Device.outputReport[1] |= (triggerIndex == 0) ? 0x08 : 0x04;
    g_Device.outputReport[offset] = mode;

    if (params != NULL && paramLen > 0) {
        int maxCopy = paramLen < 10 ? paramLen : 10;
        memcpy(&g_Device.outputReport[offset + 1], params, maxCopy);
    }

    DualSenseHid_SendOutputReportLocked();
    pthread_mutex_unlock(&g_Device.outputMutex);
}

void DualSenseHid_SetAdaptiveTriggers(uint8_t eventFlags, uint8_t typeLeft, uint8_t typeRight,
                                      const uint8_t* left, const uint8_t* right) {
    if (!g_Device.running) {
        return;
    }

    pthread_mutex_lock(&g_Device.outputMutex);
    g_Device.outputReport[0] = DUALSENSE_REPORT_ID_OUTPUT;

    if (eventFlags & 0x04) { // Right Trigger (DS_EFFECT_RIGHT_TRIGGER)
        g_Device.outputReport[1] |= 0x04;
        g_Device.outputReport[11] = typeRight;
        if (right != NULL) {
            memcpy(&g_Device.outputReport[12], right, 10);
        }
    }

    if (eventFlags & 0x08) { // Left Trigger (DS_EFFECT_LEFT_TRIGGER)
        g_Device.outputReport[1] |= 0x08;
        g_Device.outputReport[22] = typeLeft;
        if (left != NULL) {
            memcpy(&g_Device.outputReport[23], left, 10);
        }
    }

    int res = DualSenseHid_SendOutputReportLocked();
    pthread_mutex_unlock(&g_Device.outputMutex);

    LOGI("Applied Adaptive Triggers: flags=0x%02x, typeL=0x%02x, typeR=0x%02x (usb_res=%d)",
         eventFlags, typeLeft, typeRight, res);
}

void DualSenseHid_SetRgbLed(uint8_t r, uint8_t g, uint8_t b) {
    if (!g_Device.running) {
        return;
    }

    pthread_mutex_lock(&g_Device.outputMutex);
    g_Device.outputReport[0] = DUALSENSE_REPORT_ID_OUTPUT;
    g_Device.outputReport[2] |= 0x04; // Valid flags 1: LED color
    g_Device.outputReport[42] = 0x02; // Lightbar setup flag
    g_Device.outputReport[45] = r;
    g_Device.outputReport[46] = g;
    g_Device.outputReport[47] = b;
    DualSenseHid_SendOutputReportLocked();
    pthread_mutex_unlock(&g_Device.outputMutex);
}

void DualSenseHid_SetMotionEventState(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz) {
    if (g_Device.running && g_Device.controllerNumber != (int)controllerNumber) {
        return;
    }
    if (reportRateHz > 200) {
        reportRateHz = 200;
    }
    if (motionType == LI_MOTION_TYPE_ACCEL) {
        g_Device.accelReportRateHz = reportRateHz;
        LOGI("DualSense motion state updated: ACCEL rate=%u Hz", reportRateHz);
    } else if (motionType == LI_MOTION_TYPE_GYRO) {
        g_Device.gyroReportRateHz = reportRateHz;
        LOGI("DualSense motion state updated: GYRO rate=%u Hz", reportRateHz);
    }
}
