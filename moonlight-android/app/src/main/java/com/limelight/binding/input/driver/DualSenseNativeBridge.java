package com.limelight.binding.input.driver;

public class DualSenseNativeBridge {
    static {
        System.loadLibrary("moonlight-core");
    }

    public static native int nativeStart(int fd, short controllerNumber);
    public static native void nativeStop();
    public static native boolean nativeIsRunning();
    public static native void nativeSetRumble(byte left, byte right);
    public static native void nativeSetTriggerRumble(short leftTrigger, short rightTrigger);
    public static native void nativeSetLed(byte r, byte g, byte b);
    public static native int nativeSendOutputReport(byte[] report);
    public static native void nativeSetHapticsGain(int gainPercent);
}
