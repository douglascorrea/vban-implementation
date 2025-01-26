#ifndef VBAN4MAC_AUDIO_H
#define VBAN4MAC_AUDIO_H

#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>

// Audio buffer structure
typedef struct {
    int16_t* data;
    size_t size;
    size_t capacity;
    pthread_mutex_t mutex;
} audio_buffer_t;

// Global audio buffers
extern audio_buffer_t g_audio_buffer;
extern audio_buffer_t g_input_buffer;

// Function declarations
int audio_buffer_init(void);
void audio_buffer_cleanup(void);
OSStatus audio_output_init(void);
OSStatus audio_input_init(void);
void audio_cleanup(void);

// Device management functions
void audio_list_devices(void);
OSStatus audio_set_input_device(AudioDeviceID deviceID);
OSStatus audio_set_output_device(AudioDeviceID deviceID);

// Audio processing functions
void audio_process_input(const int16_t* audio_data, int num_samples, int num_channels);
void audio_buffer_add(const int16_t* data, size_t samples, int channels);

// Monitoring callbacks
typedef void (*audio_monitor_callback)(const float* samples, size_t count);
void audio_set_input_monitor(audio_monitor_callback callback);
void audio_set_output_monitor(audio_monitor_callback callback);

// Device name utility function
char* get_device_name(AudioDeviceID deviceID);

#endif /* VBAN4MAC_AUDIO_H */ 