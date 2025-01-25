#ifndef VBAN4MAC_H
#define VBAN4MAC_H

#include <stdint.h>

// VBAN Context Structure
typedef struct vban_context_t* vban_handle_t;

/**
 * Initialize VBAN with remote IP and stream name
 * @param remote_ip The IP address of the remote VBAN host
 * @param stream_name The VBAN stream name (max 16 chars)
 * @return Handle to VBAN context or NULL on error
 */
vban_handle_t vban_init(const char* remote_ip, const char* stream_name);

/**
 * Clean up VBAN resources
 * @param handle The VBAN handle to clean up
 */
void vban_cleanup(vban_handle_t handle);

/**
 * Send audio data to remote VBAN host
 * @param handle The VBAN handle
 * @param audio_data The audio samples (int16_t)
 * @param num_samples Number of samples per channel
 * @param num_channels Number of channels
 * @return 0 on success, negative value on error
 */
int vban_send_audio(vban_handle_t handle, const int16_t* audio_data, 
                   int num_samples, int num_channels);

/**
 * Check if VBAN is running
 * @param handle The VBAN handle
 * @return 1 if running, 0 if not
 */
int vban_is_running(vban_handle_t handle);

#endif /* VBAN4MAC_H */ 