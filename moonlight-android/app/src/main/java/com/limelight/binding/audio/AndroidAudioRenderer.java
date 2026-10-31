package com.limelight.binding.audio;

import android.content.Context;
import android.content.Intent;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.audiofx.AudioEffect;
import android.os.Build;

import com.limelight.LimeLog;
import com.limelight.binding.input.driver.DualSenseController;
import com.limelight.nvstream.av.audio.AudioRenderer;
import com.limelight.nvstream.jni.MoonBridge;

public class AndroidAudioRenderer implements AudioRenderer {

    private final Context context;
    private final boolean enableAudioFx;

    private AudioTrack track;
    private AudioTrack.OnRoutingChangedListener routingListener;

    public AndroidAudioRenderer(Context context, boolean enableAudioFx) {
        this.context = context;
        this.enableAudioFx = enableAudioFx;
    }

    private AudioTrack createAudioTrack(int channelConfig, int sampleRate, int bufferSize, boolean lowLatency) {
        AudioAttributes.Builder attributesBuilder = new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_GAME);
        AudioFormat format = new AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(sampleRate)
                .setChannelMask(channelConfig)
                .build();

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            // Use FLAG_LOW_LATENCY on L through N
            if (lowLatency) {
                attributesBuilder.setFlags(AudioAttributes.FLAG_LOW_LATENCY);
            }
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            AudioTrack.Builder trackBuilder = new AudioTrack.Builder()
                    .setAudioFormat(format)
                    .setAudioAttributes(attributesBuilder.build())
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .setBufferSizeInBytes(bufferSize);

            // Use PERFORMANCE_MODE_LOW_LATENCY on O and later
            if (lowLatency) {
                trackBuilder.setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY);
            }

            return trackBuilder.build();
        }
        else {
            return new AudioTrack(attributesBuilder.build(),
                    format,
                    bufferSize,
                    AudioTrack.MODE_STREAM,
                    AudioManager.AUDIO_SESSION_ID_GENERATE);
        }
    }

    @Override
    public int setup(MoonBridge.AudioConfiguration audioConfiguration, int sampleRate, int samplesPerFrame) {
        int channelConfig;
        int bytesPerFrame;

        switch (audioConfiguration.channelCount)
        {
            case 2:
                channelConfig = AudioFormat.CHANNEL_OUT_STEREO;
                break;
            case 4:
                channelConfig = AudioFormat.CHANNEL_OUT_QUAD;
                break;
            case 6:
                channelConfig = AudioFormat.CHANNEL_OUT_5POINT1;
                break;
            case 8:
                // AudioFormat.CHANNEL_OUT_7POINT1_SURROUND isn't available until Android 6.0,
                // yet the CHANNEL_OUT_SIDE_LEFT and CHANNEL_OUT_SIDE_RIGHT constants were added
                // in 5.0, so just hardcode the constant so we can work on Lollipop.
                channelConfig = 0x000018fc; // AudioFormat.CHANNEL_OUT_7POINT1_SURROUND
                break;
            default:
                LimeLog.severe("Decoder returned unhandled channel count");
                return -1;
        }

        LimeLog.info("Audio channel config: "+String.format("0x%X", channelConfig));

        bytesPerFrame = audioConfiguration.channelCount * samplesPerFrame * 2;

        // We're not supposed to request less than the minimum
        // buffer size for our buffer, but it appears that we can
        // do this on many devices and it lowers audio latency.
        // We'll try the small buffer size first and if it fails,
        // use the recommended larger buffer size.

        for (int i = 0; i < 4; i++) {
            boolean lowLatency;
            int bufferSize;

            // We will try:
            // 1) Small buffer, low latency mode
            // 2) Large buffer, low latency mode
            // 3) Small buffer, standard mode
            // 4) Large buffer, standard mode

            switch (i) {
                case 0:
                case 1:
                    lowLatency = true;
                    break;
                case 2:
                case 3:
                    lowLatency = false;
                    break;
                default:
                    // Unreachable
                    throw new IllegalStateException();
            }

            switch (i) {
                case 0:
                case 2:
                    bufferSize = bytesPerFrame * 2;
                    break;

                case 1:
                case 3:
                    // Try the larger buffer size
                    bufferSize = Math.max(AudioTrack.getMinBufferSize(sampleRate,
                            channelConfig,
                            AudioFormat.ENCODING_PCM_16BIT),
                            bytesPerFrame * 2);

                    // Round to next frame
                    bufferSize = (((bufferSize + (bytesPerFrame - 1)) / bytesPerFrame) * bytesPerFrame);
                    break;
                default:
                    // Unreachable
                    throw new IllegalStateException();
            }

            // Skip low latency options if hardware sample rate doesn't match the content
            if (AudioTrack.getNativeOutputSampleRate(AudioManager.STREAM_MUSIC) != sampleRate && lowLatency) {
                continue;
            }

            // Skip low latency options when using audio effects, since low latency mode
            // precludes the use of the audio effect pipeline (as of Android 13).
            if (enableAudioFx && lowLatency) {
                continue;
            }

            try {
                track = createAudioTrack(channelConfig, sampleRate, bufferSize, lowLatency);
                track.play();
                protectHdmiAudioRouting(track);

                // Successfully created working AudioTrack. We're done here.
                LimeLog.info("Audio track configuration: "+bufferSize+" "+lowLatency);
                break;
            } catch (Exception e) {
                // Try to release the AudioTrack if we got far enough
                e.printStackTrace();
                try {
                    if (track != null) {
                        track.release();
                        track = null;
                    }
                } catch (Exception ignored) {}
            }
        }

        if (track == null) {
            // Couldn't create any audio track for playback
            return -2;
        }

        return 0;
    }

    @Override
    public void playDecodedAudio(short[] audioData) {
        // Only queue up to 40 ms of pending audio data in addition to what AudioTrack is buffering for us.
        if (MoonBridge.getPendingAudioDuration() < 40) {
            // This will block until the write is completed. That can cause a backlog
            // of pending audio data, so we do the above check to be able to bound
            // latency at 40 ms in that situation.
            track.write(audioData, 0, audioData.length);
        }
        else {
            LimeLog.info("Too much pending audio data: " + MoonBridge.getPendingAudioDuration() +" ms");
        }
    }

    @Override
    public void start() {
        if (enableAudioFx) {
            // Open an audio effect control session to allow equalizers to apply audio effects
            Intent i = new Intent(AudioEffect.ACTION_OPEN_AUDIO_EFFECT_CONTROL_SESSION);
            i.putExtra(AudioEffect.EXTRA_AUDIO_SESSION, track.getAudioSessionId());
            i.putExtra(AudioEffect.EXTRA_PACKAGE_NAME, context.getPackageName());
            i.putExtra(AudioEffect.EXTRA_CONTENT_TYPE, AudioEffect.CONTENT_TYPE_GAME);
            context.sendBroadcast(i);
        }
    }

    @Override
    public void stop() {
        if (enableAudioFx) {
            // Close our audio effect control session when we're stopping
            Intent i = new Intent(AudioEffect.ACTION_CLOSE_AUDIO_EFFECT_CONTROL_SESSION);
            i.putExtra(AudioEffect.EXTRA_AUDIO_SESSION, track.getAudioSessionId());
            i.putExtra(AudioEffect.EXTRA_PACKAGE_NAME, context.getPackageName());
            context.sendBroadcast(i);
        }
    }

    private boolean hasDualSenseUsbConnected() {
        try {
            UsbManager usbManager = (UsbManager) context.getSystemService(Context.USB_SERVICE);
            if (usbManager != null) {
                for (UsbDevice dev : usbManager.getDeviceList().values()) {
                    if (DualSenseController.canClaimDevice(dev)) {
                        return true;
                    }
                }
            }
        } catch (Exception ignored) {}
        return false;
    }

    private boolean isDualSenseUsbAudioDevice(AudioDeviceInfo device) {
        if (device == null) {
            return false;
        }
        int type = device.getType();
        if (type != AudioDeviceInfo.TYPE_USB_DEVICE &&
            type != AudioDeviceInfo.TYPE_USB_HEADSET &&
            type != AudioDeviceInfo.TYPE_USB_ACCESSORY) {
            return false;
        }
        if (hasDualSenseUsbConnected()) {
            return true;
        }
        CharSequence name = device.getProductName();
        if (name != null) {
            String lower = name.toString().toLowerCase();
            return lower.contains("wireless controller") || lower.contains("dualsense") || lower.contains("sony");
        }
        return false;
    }

    private void protectHdmiAudioRouting(final AudioTrack audioTrack) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M || audioTrack == null) {
            return;
        }

        // Only override preferred audio device if a DualSense USB controller is actually
        // connected and hijacking audio routing. Otherwise leave Android's native stereo/Bluetooth
        // routing completely untouched.
        if (hasDualSenseUsbConnected()) {
            applyPreferredAudioOutput(audioTrack);
        }

        if (routingListener == null) {
            routingListener = new AudioTrack.OnRoutingChangedListener() {
                @Override
                public void onRoutingChanged(AudioTrack track) {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N && track != null) {
                        AudioDeviceInfo current = track.getRoutedDevice();
                        if (isDualSenseUsbAudioDevice(current)) {
                            LimeLog.warning("AudioTrack diverted to DualSense USB (" + current.getProductName() + ")! Re-enforcing HDMI/Speaker device.");
                            applyPreferredAudioOutput(track);
                        }
                    }
                }
            };
            audioTrack.addOnRoutingChangedListener(routingListener, null);
        }
    }

    private void applyPreferredAudioOutput(AudioTrack audioTrack) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M || audioTrack == null) {
            return;
        }

        AudioManager audioManager = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        if (audioManager == null) {
            return;
        }

        AudioDeviceInfo[] devices = audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS);
        if (devices == null || devices.length == 0) {
            return;
        }

        AudioDeviceInfo headphoneDevice = null;
        AudioDeviceInfo hdmiDevice = null;
        AudioDeviceInfo speakerDevice = null;

        for (AudioDeviceInfo dev : devices) {
            int type = dev.getType();
            // Priority 1: Bluetooth or wired headphones/headsets
            if (type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP ||
                type == AudioDeviceInfo.TYPE_WIRED_HEADPHONES ||
                type == AudioDeviceInfo.TYPE_WIRED_HEADSET ||
                type == 26 /* TYPE_BLE_HEADSET */ ||
                type == 27 /* TYPE_BLE_SPEAKER */) {
                headphoneDevice = dev;
                break;
            }
            // Priority 2: HDMI or HDMI ARC / eARC (Android TV / Shield TV)
            if (type == AudioDeviceInfo.TYPE_HDMI ||
                type == AudioDeviceInfo.TYPE_HDMI_ARC ||
                type == 29 /* TYPE_HDMI_EARC */) {
                if (hdmiDevice == null) {
                    hdmiDevice = dev;
                }
            }
            // Priority 3: Built-in main stereo speaker (explicitly NOT TYPE_BUILTIN_EARPIECE!)
            if (type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER) {
                if (speakerDevice == null) {
                    speakerDevice = dev;
                }
            }
        }

        AudioDeviceInfo target = (headphoneDevice != null) ? headphoneDevice :
                                 (hdmiDevice != null) ? hdmiDevice : speakerDevice;
        if (target != null) {
            boolean set = audioTrack.setPreferredDevice(target);
            LimeLog.info("Protected game audio routing to: " + target.getProductName() +
                    " [Type " + target.getType() + "], setPreferredDevice=" + set);
        }
    }

    @Override
    public void cleanup() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && routingListener != null && track != null) {
            try {
                track.removeOnRoutingChangedListener(routingListener);
            } catch (Exception ignored) {}
            routingListener = null;
        }

        // Immediately drop all pending data
        track.pause();
        track.flush();

        track.release();
    }
}
