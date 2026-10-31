package com.limelight.binding.input.driver;

import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.os.SystemClock;

import com.limelight.LimeLog;
import com.limelight.nvstream.input.ControllerPacket;
import com.limelight.nvstream.jni.MoonBridge;

import java.util.Arrays;

public class DualSenseController extends AbstractController {

    public static final int VENDOR_ID = 0x054c;
    public static final int PRODUCT_ID_DUALSENSE = 0x0ce6;
    public static final int PRODUCT_ID_DUALSENSE_EDGE = 0x0df2;

    private static final int PACKET_SIZE = 64;
    private static final byte REPORT_ID_INPUT = 0x01;
    private static final byte REPORT_ID_OUTPUT = 0x02;

    private static volatile DualSenseController activeInstance;

    private final UsbDevice device;
    private final UsbDeviceConnection connection;
    private UsbEndpoint inEndpt;
    private UsbEndpoint outEndpt;
    private UsbInterface hidInterface;
    private Thread inputThread;
    private volatile boolean stopped = false;

    private final byte[] outputReport = new byte[PACKET_SIZE];
    private long lastMotionReportTime = 0;
    private byte ledR = 0, ledG = 0, ledB = (byte) 255; // Default blue
    private volatile short assignedControllerNumber = 0;

    public void setAssignedControllerNumber(short controllerNumber) {
        this.assignedControllerNumber = controllerNumber;
    }

    public short getAssignedControllerNumber() {
        return assignedControllerNumber;
    }

    public static boolean canClaimDevice(UsbDevice device) {
        if (device == null) {
            return false;
        }
        return device.getVendorId() == VENDOR_ID &&
                (device.getProductId() == PRODUCT_ID_DUALSENSE || device.getProductId() == PRODUCT_ID_DUALSENSE_EDGE);
    }

    public static DualSenseController getActiveInstance() {
        return activeInstance;
    }

    public UsbDevice getDevice() {
        return device;
    }

    public UsbDeviceConnection getConnection() {
        return connection;
    }

    public int getUsbFileDescriptor() {
        return connection != null ? connection.getFileDescriptor() : -1;
    }

    public DualSenseController(UsbDevice device, UsbDeviceConnection connection, int deviceId, UsbDriverListener listener) {
        super(deviceId, listener, device.getVendorId(), device.getProductId());
        this.device = device;
        this.connection = connection;
        this.type = MoonBridge.LI_CTYPE_PS;
        this.capabilities = (short) (MoonBridge.LI_CCAP_ANALOG_TRIGGERS |
                MoonBridge.LI_CCAP_RUMBLE |
                MoonBridge.LI_CCAP_TRIGGER_RUMBLE |
                MoonBridge.LI_CCAP_TOUCHPAD |
                MoonBridge.LI_CCAP_GYRO |
                MoonBridge.LI_CCAP_ACCEL |
                MoonBridge.LI_CCAP_BATTERY_STATE |
                MoonBridge.LI_CCAP_RGB_LED |
                MoonBridge.LI_CCAP_HAPTICS_PCM);
        this.supportedButtonFlags = 0xFFFFFFFF;
    }

    @Override
    public boolean start() {
        LimeLog.info("DualSenseController: Starting USB userspace driver for " + device.getDeviceName());

        // Claim all interfaces with force=true to detach any Android kernel drivers (HID, Audio, etc.)
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            UsbInterface iface = device.getInterface(i);
            boolean claimed = connection.claimInterface(iface, true);
            if (!claimed) {
                LimeLog.warning("DualSense: Could not claim interface " + i + " (class " + iface.getInterfaceClass() + ")");
            } else {
                LimeLog.info("DualSense: Successfully claimed interface " + i + " (class " + iface.getInterfaceClass() + ")");
            }

            if (iface.getInterfaceClass() == UsbConstants.USB_CLASS_HID) {
                hidInterface = iface;
            }
        }

        // Fallback: DualSense standard HID is interface 3 if class wasn't reported as USB_CLASS_HID
        if (hidInterface == null && device.getInterfaceCount() > 3) {
            hidInterface = device.getInterface(3);
        }

        if (hidInterface == null) {
            LimeLog.warning("DualSense: HID interface not found!");
            return false;
        }

        for (int i = 0; i < hidInterface.getEndpointCount(); i++) {
            UsbEndpoint ep = hidInterface.getEndpoint(i);
            if (ep.getDirection() == UsbConstants.USB_DIR_IN) {
                inEndpt = ep;
            } else if (ep.getDirection() == UsbConstants.USB_DIR_OUT) {
                outEndpt = ep;
            }
        }

        if (inEndpt == null) {
            LimeLog.warning("DualSense: Interrupt IN endpoint not found!");
            return false;
        }

        activeInstance = this;

        // Notify listener first so ControllerHandler assigns controllerNumber and sends ControllerArrivalEvent
        // before the native 1000 Hz HID thread starts sending MultiController input packets.
        notifyDeviceAdded();

        // Try initializing native 1000 Hz HID engine via JNI
        int fd = connection.getFileDescriptor();
        boolean nativeActive = false;
        if (fd >= 0) {
            int ret = DualSenseNativeBridge.nativeStart(fd, assignedControllerNumber);
            if (ret == 0) {
                nativeActive = true;
                LimeLog.info("DualSenseController: Native 1000 Hz HID engine successfully started for fd " + fd + " (controller " + assignedControllerNumber + ")");
            } else {
                LimeLog.warning("DualSenseController: nativeStart returned " + ret + ", falling back to Java thread");
            }
        }

        // Initialize output report with default state (lightbar blue)
        synchronized (outputReport) {
            Arrays.fill(outputReport, (byte) 0);
            outputReport[0] = REPORT_ID_OUTPUT;
            outputReport[1] = (byte) 0x03; // Valid flags 0: Rumble emulation & haptics enable
            outputReport[2] = (byte) 0x14; // Valid flags 1: LED color update & player LEDs
            outputReport[39] = (byte) 0x02; // Player LED brightness: low/medium
            outputReport[42] = (byte) 0x02; // Lightbar setup flag
            outputReport[44] = (byte) 0x04; // Player 1 LED (center)
            outputReport[45] = ledR;
            outputReport[46] = ledG;
            outputReport[47] = ledB;
            if (!nativeActive) {
                sendOutputReport();
            }
        }

        if (!nativeActive) {
            inputThread = createInputThread();
            inputThread.start();
        }

        return true;
    }

    private Thread createInputThread() {
        return new Thread(() -> {
            byte[] buffer = new byte[PACKET_SIZE];
            int errorCount = 0;

            while (!Thread.currentThread().isInterrupted() && !stopped) {
                int res = connection.bulkTransfer(inEndpt, buffer, buffer.length, 1000);
                if (res > 0) {
                    errorCount = 0;
                    if (handleRead(buffer, res)) {
                        reportInput();

                        long now = SystemClock.uptimeMillis();
                        if (now - lastMotionReportTime >= 5) { // Cap motion reports at ~200 Hz
                            lastMotionReportTime = now;
                            reportMotion();
                        }
                    }
                } else if (res < 0) {
                    if (stopped || Thread.currentThread().isInterrupted()) {
                        break;
                    }
                    errorCount++;
                    if (errorCount > 10) {
                        LimeLog.warning("DualSense: Persistent I/O error on USB IN endpoint, stopping driver");
                        break;
                    }
                }
            }

            DualSenseController.this.stop();
        }, "DualSense-InputThread");
    }

    private boolean handleRead(byte[] buffer, int length) {
        if (length < 11 || buffer[0] != REPORT_ID_INPUT) {
            return false;
        }

        // Analog Sticks (0..255, 128 center; ControllerHandler inverts Y)
        leftStickX = Math.max(-1.0f, Math.min(1.0f, ((buffer[1] & 0xFF) - 128) / 127.0f));
        leftStickY = Math.max(-1.0f, Math.min(1.0f, ((buffer[2] & 0xFF) - 128) / 127.0f));
        rightStickX = Math.max(-1.0f, Math.min(1.0f, ((buffer[3] & 0xFF) - 128) / 127.0f));
        rightStickY = Math.max(-1.0f, Math.min(1.0f, ((buffer[4] & 0xFF) - 128) / 127.0f));

        // Triggers (0..255)
        leftTrigger = (buffer[5] & 0xFF) / 255.0f;
        rightTrigger = (buffer[6] & 0xFF) / 255.0f;

        // D-Pad Hat Switch (buffer[8] lower 4 bits; buffer[7] is seq_number!)
        int dpad = buffer[8] & 0x0F;
        boolean up = (dpad == 0 || dpad == 1 || dpad == 7);
        boolean right = (dpad == 1 || dpad == 2 || dpad == 3);
        boolean down = (dpad == 3 || dpad == 4 || dpad == 5);
        boolean left = (dpad == 5 || dpad == 6 || dpad == 7);

        buttonFlags = 0;
        setButtonFlag(ControllerPacket.UP_FLAG, up ? 1 : 0);
        setButtonFlag(ControllerPacket.RIGHT_FLAG, right ? 1 : 0);
        setButtonFlag(ControllerPacket.DOWN_FLAG, down ? 1 : 0);
        setButtonFlag(ControllerPacket.LEFT_FLAG, left ? 1 : 0);

        // Action Buttons (buffer[8])
        int b8 = buffer[8] & 0xFF;
        setButtonFlag(ControllerPacket.X_FLAG, b8 & 0x10); // Square
        setButtonFlag(ControllerPacket.A_FLAG, b8 & 0x20); // Cross
        setButtonFlag(ControllerPacket.B_FLAG, b8 & 0x40); // Circle
        setButtonFlag(ControllerPacket.Y_FLAG, b8 & 0x80); // Triangle

        // Shoulder & Center Buttons (buffer[9])
        int b9 = buffer[9] & 0xFF;
        setButtonFlag(ControllerPacket.LB_FLAG, b9 & 0x01); // L1
        setButtonFlag(ControllerPacket.RB_FLAG, b9 & 0x02); // R1
        setButtonFlag(ControllerPacket.BACK_FLAG, b9 & 0x10); // Create / Share
        setButtonFlag(ControllerPacket.PLAY_FLAG, b9 & 0x20); // Options
        setButtonFlag(ControllerPacket.LS_CLK_FLAG, b9 & 0x40); // L3
        setButtonFlag(ControllerPacket.RS_CLK_FLAG, b9 & 0x80); // R3

        // System Buttons (buffer[10])
        int b10 = buffer[10] & 0xFF;
        setButtonFlag(ControllerPacket.SPECIAL_BUTTON_FLAG, b10 & 0x01); // PS Button
        setButtonFlag(ControllerPacket.TOUCHPAD_FLAG, b10 & 0x02); // Touchpad Click
        setButtonFlag(ControllerPacket.MISC_FLAG, b10 & 0x04); // Mic Mute Button

        // Motion Sensors (Gyroscope & Accelerometer)
        if (length >= 28) {
            short rawGyroX = (short) ((buffer[16] & 0xFF) | ((buffer[17] & 0xFF) << 8));
            short rawGyroY = (short) ((buffer[18] & 0xFF) | ((buffer[19] & 0xFF) << 8));
            short rawGyroZ = (short) ((buffer[20] & 0xFF) | ((buffer[21] & 0xFF) << 8));
            short rawAccelX = (short) ((buffer[22] & 0xFF) | ((buffer[23] & 0xFF) << 8));
            short rawAccelY = (short) ((buffer[24] & 0xFF) | ((buffer[25] & 0xFF) << 8));
            short rawAccelZ = (short) ((buffer[26] & 0xFF) | ((buffer[27] & 0xFF) << 8));

            // Calibration scale factors
            gyroX = rawGyroX / 16.0f;
            gyroY = rawGyroY / 16.0f;
            gyroZ = rawGyroZ / 16.0f;
            accelX = rawAccelX / 8192.0f;
            accelY = rawAccelY / 8192.0f;
            accelZ = rawAccelZ / 8192.0f;
        }

        return true;
    }

    @Override
    public void rumble(short lowFreqMotor, short highFreqMotor) {
        if (DualSenseNativeBridge.nativeIsRunning()) {
            byte right = (byte) (((highFreqMotor & 0xFFFF) * 255) / 65535);
            byte left = (byte) (((lowFreqMotor & 0xFFFF) * 255) / 65535);
            DualSenseNativeBridge.nativeSetRumble(left, right);
            return;
        }

        synchronized (outputReport) {
            outputReport[0] = REPORT_ID_OUTPUT;
            outputReport[1] |= (byte) 0x03; // Valid flags 0: Enable rumble emulation & haptic rumble
            outputReport[3] = (byte) (((highFreqMotor & 0xFFFF) * 255) / 65535); // Right motor (high freq)
            outputReport[4] = (byte) (((lowFreqMotor & 0xFFFF) * 255) / 65535);  // Left motor (low freq)
            sendOutputReport();
        }
    }

    @Override
    public void rumbleTriggers(short leftTrigger, short rightTrigger) {
        if (DualSenseNativeBridge.nativeIsRunning()) {
            DualSenseNativeBridge.nativeSetTriggerRumble(leftTrigger, rightTrigger);
            return;
        }

        synchronized (outputReport) {
            outputReport[0] = REPORT_ID_OUTPUT;
            outputReport[1] |= (byte) 0x0C; // Valid flags 0: Enable left (0x08) & right (0x04) trigger effects

            // Right Trigger effect
            if (rightTrigger > 0) {
                byte force = (byte) Math.max(1, ((rightTrigger & 0xFFFF) * 7) / 65535);
                outputReport[11] = 0x01; // Continuous resistance mode
                outputReport[12] = 0x00; // Start position
                outputReport[13] = force; // Force
            } else {
                outputReport[11] = 0x00; // Off
                outputReport[12] = 0x00;
                outputReport[13] = 0x00;
            }

            // Left Trigger effect
            if (leftTrigger > 0) {
                byte force = (byte) Math.max(1, ((leftTrigger & 0xFFFF) * 7) / 65535);
                outputReport[22] = 0x01; // Continuous resistance mode
                outputReport[23] = 0x00; // Start position
                outputReport[24] = force; // Force
            } else {
                outputReport[22] = 0x00; // Off
                outputReport[23] = 0x00;
                outputReport[24] = 0x00;
            }

            sendOutputReport();
        }
    }

    @Override
    public void setLed(byte r, byte g, byte b) {
        this.ledR = r;
        this.ledG = g;
        this.ledB = b;

        if (DualSenseNativeBridge.nativeIsRunning()) {
            DualSenseNativeBridge.nativeSetLed(r, g, b);
            return;
        }

        synchronized (outputReport) {
            outputReport[0] = REPORT_ID_OUTPUT;
            outputReport[2] |= (byte) 0x04; // Valid flags 1: LED color
            outputReport[42] = (byte) 0x02; // Lightbar setup flag
            outputReport[45] = r;
            outputReport[46] = g;
            outputReport[47] = b;
            sendOutputReport();
        }
    }

    private void sendOutputReport() {
        if (outEndpt != null && connection != null && !stopped) {
            outputReport[45] = ledR;
            outputReport[46] = ledG;
            outputReport[47] = ledB;
            connection.bulkTransfer(outEndpt, outputReport, outputReport.length, 100);
        }
    }

    @Override
    public void stop() {
        if (stopped) {
            return;
        }
        stopped = true;

        if (activeInstance == this) {
            activeInstance = null;
        }

        if (DualSenseNativeBridge.nativeIsRunning()) {
            DualSenseNativeBridge.nativeStop();
        }

        // Turn off motors and triggers in Java fallback
        synchronized (outputReport) {
            outputReport[3] = 0;
            outputReport[4] = 0;
            outputReport[11] = 0;
            outputReport[22] = 0;
            sendOutputReport();
        }

        if (inputThread != null) {
            inputThread.interrupt();
            inputThread = null;
        }

        notifyDeviceRemoved();
    }
}
