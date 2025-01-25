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

// Audio initialization functions
OSStatus audio_output_init(void);
OSStatus audio_input_init(void);
int audio_buffer_init(void);

// Audio processing functions
void audio_process_input(const int16_t* audio_data, int num_samples, int num_channels);
void audio_buffer_add(const int16_t* data, size_t samples, int channels);

// Audio cleanup functions
void audio_cleanup(void);

#endif /* VBAN4MAC_AUDIO_H */ 