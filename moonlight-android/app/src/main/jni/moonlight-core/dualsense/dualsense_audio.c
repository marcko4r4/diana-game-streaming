#include "dualsense_audio.h"
#include "ring_buffer.h"

#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <android/log.h>

#define TAG "DualSenseAudioISO"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

typedef struct {
    int fd;
    int controllerNumber;
    volatile bool running;
    volatile bool shouldStop;
    volatile bool urbsSubmitted;
    int gainPercent;
    int consecutiveSilentUrbs;
    pthread_t workerThread;
    pthread_mutex_t wakeMutex;
    pthread_cond_t wakeCond;
    struct usbdevfs_urb* urbs[DUALSENSE_NUM_URBS];
    uint8_t* urbBuffers[DUALSENSE_NUM_URBS];
} DualSenseAudioState;

static DualSenseAudioState g_AudioState = {
    .fd = -1,
    .controllerNumber = -1,
    .running = false,
    .shouldStop = false,
    .urbsSubmitted = false,
    .gainPercent = 100,
    .consecutiveSilentUrbs = 0,
    .wakeMutex = PTHREAD_MUTEX_INITIALIZER,
    .wakeCond = PTHREAD_COND_INITIALIZER
};

static HapticsRingBuffer g_RingBuffer;

static void DualSenseAudio_FillUrbFromRingBuffer(DualSenseAudioState* dev, struct usbdevfs_urb* urb, size_t framesPerUrb) {
    HapticsFrame* frames = (HapticsFrame*)urb->buffer;
    size_t readCount = HapticsRingBuffer_PopFrames(&g_RingBuffer, frames, framesPerUrb);

    // Check if all read samples are zero (silence)
    bool hasSignal = false;
    for (size_t i = 0; i < readCount; i++) {
        if (frames[i].ch[2] != 0 || frames[i].ch[3] != 0) {
            hasSignal = true;
            break;
        }
    }

    if (hasSignal) {
        dev->consecutiveSilentUrbs = 0;
        int gain = dev->gainPercent;
        if (gain != 100 && gain > 0) {
            for (size_t i = 0; i < readCount; i++) {
                int32_t l = ((int32_t)frames[i].ch[2] * gain) / 100;
                int32_t r = ((int32_t)frames[i].ch[3] * gain) / 100;
                frames[i].ch[2] = (int16_t)(l > 32767 ? 32767 : (l < -32768 ? -32768 : l));
                frames[i].ch[3] = (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
            }
        }
    } else {
        dev->consecutiveSilentUrbs++;
    }

    for (int p = 0; p < DUALSENSE_ISO_PACKETS_PER_URB; p++) {
        urb->iso_frame_desc[p].length = DUALSENSE_ISO_PACKET_SIZE;
        urb->iso_frame_desc[p].actual_length = 0;
        urb->iso_frame_desc[p].status = 0;
    }
    urb->status = 0;
}

static void* DualSenseAudio_WorkerThread(void* arg) {
    DualSenseAudioState* dev = (DualSenseAudioState*)arg;
    size_t framesPerUrb = DUALSENSE_FRAMES_PER_MS * DUALSENSE_ISO_PACKETS_PER_URB; // 192 frames (1536 bytes)
    int activeUrbs = 0;

    LOGI("DualSense ISO worker thread running (on-demand EP 0x01, 48kHz, 4-ch, 4 URBs pipeline)");

    while (!dev->shouldStop) {
        if (activeUrbs == 0) {
            // Wait until non-silent haptic samples arrive in the ring buffer
            pthread_mutex_lock(&dev->wakeMutex);
            while (!dev->shouldStop && HapticsRingBuffer_AvailableRead(&g_RingBuffer) == 0) {
                pthread_cond_wait(&dev->wakeCond, &dev->wakeMutex);
            }
            pthread_mutex_unlock(&dev->wakeMutex);

            if (dev->shouldStop) {
                break;
            }

            // Prime and submit all 4 URBs to kick off the 16 ms ISO pipeline
            dev->consecutiveSilentUrbs = 0;
            for (int i = 0; i < DUALSENSE_NUM_URBS; i++) {
                DualSenseAudio_FillUrbFromRingBuffer(dev, dev->urbs[i], framesPerUrb);
                if (ioctl(dev->fd, USBDEVFS_SUBMITURB, dev->urbs[i]) == 0) {
                    activeUrbs++;
                } else if (!dev->shouldStop) {
                    LOGW("Initial submit of ISO URB %d failed: %s", i, strerror(errno));
                }
            }
            dev->urbsSubmitted = (activeUrbs > 0);
            if (activeUrbs == 0) {
                usleep(5000);
            }
            continue;
        }

        struct usbdevfs_urb* reaped_urb = NULL;
        int res = ioctl(dev->fd, USBDEVFS_REAPURB, &reaped_urb);
        if (res < 0) {
            if (dev->shouldStop) {
                break;
            }
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            if (errno == ENODEV || errno == ESHUTDOWN) {
                LOGW("USB audio device disconnected (errno=%d)", errno);
                break;
            }
            LOGW("USBDEVFS_REAPURB error: %s (errno=%d)", strerror(errno), errno);
            usleep(1000);
            continue;
        }

        if (reaped_urb != NULL) {
            activeUrbs--;
            if (dev->shouldStop) {
                break;
            }

            DualSenseAudio_FillUrbFromRingBuffer(dev, reaped_urb, framesPerUrb);

            // If we have had > 50 consecutive silent URBs (~200 ms of silence) and the ring buffer is empty,
            // let the in-flight URBs drain naturally so the USB bus is completely idle when haptics are silent.
            if (dev->consecutiveSilentUrbs > 50 && HapticsRingBuffer_AvailableRead(&g_RingBuffer) == 0) {
                if (activeUrbs == 0) {
                    dev->urbsSubmitted = false;
                }
                continue;
            }

            if (ioctl(dev->fd, USBDEVFS_SUBMITURB, reaped_urb) == 0) {
                activeUrbs++;
            } else if (!dev->shouldStop) {
                LOGW("USBDEVFS_SUBMITURB ISO resubmit error: %s", strerror(errno));
            }
        }
    }

    LOGI("DualSense ISO worker thread stopped");
    return NULL;
}

int DualSenseAudio_Start(int fd, int controllerNumber) {
    if (fd < 0) {
        return -EINVAL;
    }

    if (g_AudioState.running) {
        DualSenseAudio_Stop();
    }

    LOGI("Starting DualSense native ISO audio stream for fd=%d, controller=%d", fd, controllerNumber);

    g_AudioState.fd = fd;
    g_AudioState.controllerNumber = controllerNumber;
    g_AudioState.shouldStop = false;

    // 1. Claim Interface 1 (Audio Streaming)
    int iface = DUALSENSE_AUDIO_INTERFACE;
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0) {
        struct usbdevfs_disconnect_claim dc;
        memset(&dc, 0, sizeof(dc));
        dc.interface = DUALSENSE_AUDIO_INTERFACE;
        dc.flags = USBDEVFS_DISCONNECT_CLAIM_EXCEPT_DRIVER;
        strcpy(dc.driver, "usbfs");
        ioctl(fd, USBDEVFS_DISCONNECT_CLAIM, &dc);
    }

    // 2. Set Alt Setting 1 (4 channels, 48 kHz, 16-bit PCM)
    struct usbdevfs_setinterface setiface;
    memset(&setiface, 0, sizeof(setiface));
    setiface.interface = DUALSENSE_AUDIO_INTERFACE;
    setiface.altsetting = DUALSENSE_AUDIO_ALTSETTING;
    if (ioctl(fd, USBDEVFS_SETINTERFACE, &setiface) < 0) {
        LOGW("USBDEVFS_SETINTERFACE 1 alt 1 warning: %s", strerror(errno));
    } else {
        LOGI("DualSense Interface 1 configured to Alt Setting 1 (4ch 48kHz S16LE)");
    }

    // 3. Initialize ring buffer with silence
    HapticsRingBuffer_Init(&g_RingBuffer);

    // 4. Allocate and submit initial Isochronous URBs pipeline
    size_t urbBufferSize = DUALSENSE_ISO_PACKETS_PER_URB * DUALSENSE_ISO_PACKET_SIZE; // 1536 bytes
    size_t urbStructSize = sizeof(struct usbdevfs_urb) + sizeof(struct usbdevfs_iso_packet_desc) * DUALSENSE_ISO_PACKETS_PER_URB;

    for (int i = 0; i < DUALSENSE_NUM_URBS; i++) {
        g_AudioState.urbs[i] = (struct usbdevfs_urb*)malloc(urbStructSize);
        g_AudioState.urbBuffers[i] = (uint8_t*)calloc(1, urbBufferSize);

        if (!g_AudioState.urbs[i] || !g_AudioState.urbBuffers[i]) {
            LOGE("Failed to allocate ISO URB %d", i);
            DualSenseAudio_Stop();
            return -ENOMEM;
        }

        memset(g_AudioState.urbs[i], 0, urbStructSize);
        g_AudioState.urbs[i]->type = USBDEVFS_URB_TYPE_ISO;
        g_AudioState.urbs[i]->endpoint = DUALSENSE_EP_ISO_OUT;
        g_AudioState.urbs[i]->flags = USBDEVFS_URB_ISO_ASAP;
        g_AudioState.urbs[i]->buffer = g_AudioState.urbBuffers[i];
        g_AudioState.urbs[i]->buffer_length = (int)urbBufferSize;
        g_AudioState.urbs[i]->number_of_packets = DUALSENSE_ISO_PACKETS_PER_URB;
        g_AudioState.urbs[i]->usercontext = &g_AudioState;

        for (int p = 0; p < DUALSENSE_ISO_PACKETS_PER_URB; p++) {
            g_AudioState.urbs[i]->iso_frame_desc[p].length = DUALSENSE_ISO_PACKET_SIZE;
        }
    }

    g_AudioState.urbsSubmitted = false;
    g_AudioState.consecutiveSilentUrbs = 0;
    g_AudioState.running = true;

    // 5. Start worker thread (sleeps on condition variable until haptic PCM packets arrive)
    if (pthread_create(&g_AudioState.workerThread, NULL, DualSenseAudio_WorkerThread, &g_AudioState) != 0) {
        LOGE("Failed to start ISO audio worker thread: %s", strerror(errno));
        DualSenseAudio_Stop();
        return -errno;
    }

    LOGI("DualSense native ISO audio engine active (on-demand)");
    return 0;
}

void DualSenseAudio_Stop(void) {
    if (!g_AudioState.running) {
        return;
    }

    LOGI("Stopping DualSense native ISO audio engine");
    pthread_mutex_lock(&g_AudioState.wakeMutex);
    g_AudioState.shouldStop = true;
    g_AudioState.running = false;
    pthread_cond_broadcast(&g_AudioState.wakeCond);
    pthread_mutex_unlock(&g_AudioState.wakeMutex);

    // Discard any active URBs
    if (g_AudioState.fd >= 0 && g_AudioState.urbsSubmitted) {
        for (int i = 0; i < DUALSENSE_NUM_URBS; i++) {
            if (g_AudioState.urbs[i]) {
                ioctl(g_AudioState.fd, USBDEVFS_DISCARDURB, g_AudioState.urbs[i]);
            }
        }
    }

    pthread_join(g_AudioState.workerThread, NULL);

    // Free URB memory
    for (int i = 0; i < DUALSENSE_NUM_URBS; i++) {
        if (g_AudioState.urbs[i]) {
            free(g_AudioState.urbs[i]);
            g_AudioState.urbs[i] = NULL;
        }
        if (g_AudioState.urbBuffers[i]) {
            free(g_AudioState.urbBuffers[i]);
            g_AudioState.urbBuffers[i] = NULL;
        }
    }

    // Revert Interface 1 to Alt Setting 0 (Zero bandwidth) and release
    if (g_AudioState.fd >= 0) {
        struct usbdevfs_setinterface setiface;
        memset(&setiface, 0, sizeof(setiface));
        setiface.interface = DUALSENSE_AUDIO_INTERFACE;
        setiface.altsetting = 0;
        ioctl(g_AudioState.fd, USBDEVFS_SETINTERFACE, &setiface);

        int iface = DUALSENSE_AUDIO_INTERFACE;
        ioctl(g_AudioState.fd, USBDEVFS_RELEASEINTERFACE, &iface);
    }

    g_AudioState.fd = -1;
    g_AudioState.controllerNumber = -1;
    LOGI("DualSense native ISO audio engine stopped cleanly");
}

bool DualSenseAudio_IsRunning(void) {
    return g_AudioState.running;
}

void DualSenseAudio_FeedHapticsPcm(uint16_t controllerNumber, uint32_t sequence, const uint8_t* pcm, uint16_t frameCount) {
    if (!g_AudioState.running || g_AudioState.controllerNumber != controllerNumber) {
        return;
    }
    if (pcm == NULL || frameCount == 0) {
        return;
    }

    const int16_t* samples = (const int16_t*)pcm;
    if (!g_AudioState.urbsSubmitted) {
        // When idle, ignore all-zero silence packets so we don't wake up the USB ISO pipeline unnecessarily
        bool hasSignal = false;
        size_t totalSamples = (size_t)frameCount * 2;
        for (size_t i = 0; i < totalSamples; i++) {
            if (samples[i] != 0) {
                hasSignal = true;
                break;
            }
        }
        if (!hasSignal) {
            return;
        }
    }

    HapticsRingBuffer_PushStereo(&g_RingBuffer, samples, frameCount);

    if (!g_AudioState.urbsSubmitted) {
        pthread_mutex_lock(&g_AudioState.wakeMutex);
        pthread_cond_signal(&g_AudioState.wakeCond);
        pthread_mutex_unlock(&g_AudioState.wakeMutex);
    }
}

void DualSenseAudio_SetGain(int gainPercent) {
    if (gainPercent < 10) gainPercent = 10;
    if (gainPercent > 200) gainPercent = 200;
    g_AudioState.gainPercent = gainPercent;
}
