#ifndef DUALSENSE_AUDIO_H
#define DUALSENSE_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DUALSENSE_AUDIO_INTERFACE      1
#define DUALSENSE_AUDIO_ALTSETTING     1
#define DUALSENSE_EP_ISO_OUT           0x01

#define DUALSENSE_AUDIO_SAMPLE_RATE    48000
#define DUALSENSE_AUDIO_CHANNELS       4
#define DUALSENSE_AUDIO_FRAME_BYTES    8  // 4 ch * 2 bytes (S16LE)
#define DUALSENSE_FRAMES_PER_MS        48 // 48000 / 1000
#define DUALSENSE_ISO_PACKET_SIZE      (DUALSENSE_FRAMES_PER_MS * DUALSENSE_AUDIO_FRAME_BYTES) // 384 bytes

#define DUALSENSE_ISO_PACKETS_PER_URB  4  // 4 ms per URB
#define DUALSENSE_NUM_URBS             4  // 4 URBs in flight (16 ms pipeline)

/**
 * Start native Isochronous audio streaming engine on Interface 1, Alt Setting 1.
 * @param fd USB device file descriptor.
 * @param controllerNumber Moonlight controller index.
 * @return 0 on success, negative error code on failure.
 */
int DualSenseAudio_Start(int fd, int controllerNumber);

/**
 * Stop native Isochronous audio streaming engine and revert Interface 1 to Alt Setting 0.
 */
void DualSenseAudio_Stop(void);

/**
 * Check if the Isochronous audio streaming engine is running.
 */
bool DualSenseAudio_IsRunning(void);

/**
 * Push S16LE stereo haptic samples from Vibepollo (e.g., from 0x5601 packet).
 * @param controllerNumber Controller index.
 * @param sequence Packet sequence counter.
 * @param pcm Stereo S16LE PCM samples.
 * @param frameCount Number of stereo frames (e.g., 240).
 */
void DualSenseAudio_FeedHapticsPcm(uint16_t controllerNumber, uint32_t sequence, const uint8_t* pcm, uint16_t frameCount);

/**
 * Set haptics gain factor (50 to 200, default 100).
 */
void DualSenseAudio_SetGain(int gainPercent);

#ifdef __cplusplus
}
#endif

#endif // DUALSENSE_AUDIO_H
