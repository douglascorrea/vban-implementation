#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <libkern/OSByteOrder.h>
#include "audio.h"
#include "../include/vban4mac/types.h"

#define AUDIO_BUFFER_SIZE (VBAN_PROTOCOL_MAXNBS * 16)  // Buffer for ~256ms of audio

// Audio Unit globals
static AudioComponentInstance audio_unit = NULL;
static AudioComponent output_component = NULL;
static AudioComponentInstance input_unit = NULL;
static AudioComponent input_component = NULL;

// Audio buffers
static audio_buffer_t g_audio_buffer = {0};
static audio_buffer_t g_input_buffer = {0};

// Audio callbacks
static OSStatus audio_render_callback(void *inRefCon,
                                    AudioUnitRenderActionFlags *ioActionFlags,
                                    const AudioTimeStamp *inTimeStamp,
                                    UInt32 inBusNumber,
                                    UInt32 inNumberFrames,
                                    AudioBufferList *ioData) {
    pthread_mutex_lock(&g_audio_buffer.mutex);
    
    // Get pointers to left and right channel buffers
    int16_t* left = (int16_t*)ioData->mBuffers[0].mData;
    int16_t* right = (int16_t*)ioData->mBuffers[1].mData;
    size_t frames_to_copy = inNumberFrames;
    
    if (g_audio_buffer.size >= frames_to_copy * 2) {  // 2 channels
        // Deinterleave and copy data
        for (size_t i = 0; i < frames_to_copy; i++) {
            left[i] = g_audio_buffer.data[i * 2];
            right[i] = g_audio_buffer.data[i * 2 + 1];
        }
        
        // Remove processed data
        memmove(g_audio_buffer.data,
                g_audio_buffer.data + frames_to_copy * 2,
                (g_audio_buffer.size - frames_to_copy * 2) * sizeof(int16_t));
        g_audio_buffer.size -= frames_to_copy * 2;
    } else {
        // Not enough data, output silence
        memset(left, 0, frames_to_copy * sizeof(int16_t));
        memset(right, 0, frames_to_copy * sizeof(int16_t));
    }
    
    pthread_mutex_unlock(&g_audio_buffer.mutex);
    return noErr;
}

static OSStatus audio_input_callback(void *inRefCon,
                                   AudioUnitRenderActionFlags *ioActionFlags,
                                   const AudioTimeStamp *inTimeStamp,
                                   UInt32 inBusNumber,
                                   UInt32 inNumberFrames,
                                   AudioBufferList *ioData) {
    // Create buffer list for rendered audio
    AudioBufferList buffer_list;
    buffer_list.mNumberBuffers = 1;
    buffer_list.mBuffers[0].mNumberChannels = 1;  // Mono input
    buffer_list.mBuffers[0].mDataByteSize = inNumberFrames * sizeof(float);
    buffer_list.mBuffers[0].mData = malloc(buffer_list.mBuffers[0].mDataByteSize);

    if (!buffer_list.mBuffers[0].mData) {
        printf("Failed to allocate buffer for input callback\n");
        return -1;
    }

    // Render the audio data
    OSStatus status = AudioUnitRender(input_unit,
                                    ioActionFlags,
                                    inTimeStamp,
                                    inBusNumber,
                                    inNumberFrames,
                                    &buffer_list);

    if (status == noErr) {
        pthread_mutex_lock(&g_input_buffer.mutex);
        
        // Convert float mono to int16 stereo
        float* input_samples = (float*)buffer_list.mBuffers[0].mData;
        size_t output_samples = inNumberFrames * 2; // Stereo output
        
        if (g_input_buffer.size + output_samples <= g_input_buffer.capacity) {
            // Convert and duplicate mono to stereo
            for (size_t i = 0; i < inNumberFrames; i++) {
                // Convert float (-1.0 to 1.0) to int16 (-32768 to 32767)
                int16_t sample = (int16_t)(input_samples[i] * 32767.0f);
                // Write same sample to both channels
                g_input_buffer.data[g_input_buffer.size + i * 2] = sample;
                g_input_buffer.data[g_input_buffer.size + i * 2 + 1] = sample;
            }
            
            g_input_buffer.size += output_samples;
        }
        
        pthread_mutex_unlock(&g_input_buffer.mutex);
    }

    free(buffer_list.mBuffers[0].mData);
    return status;
}

// Audio initialization functions
OSStatus audio_output_init(void) {
    // Describe audio component
    AudioComponentDescription desc = {0};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    // Get default output component
    output_component = AudioComponentFindNext(NULL, &desc);
    if (!output_component) {
        printf("Failed to find default output component\n");
        return -1;
    }

    // Create audio unit instance
    OSStatus status = AudioComponentInstanceNew(output_component, &audio_unit);
    if (status != noErr) {
        printf("Failed to create audio unit instance\n");
        return status;
    }

    // Set up stream format
    AudioStreamBasicDescription format = {0};
    format.mSampleRate = VBAN_SAMPLE_RATE;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsSignedInteger | 
                         kAudioFormatFlagIsPacked |
                         kAudioFormatFlagIsNonInterleaved;
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = 2; // Stereo
    format.mBitsPerChannel = 16;  // 16-bit PCM
    format.mBytesPerPacket = format.mBytesPerFrame = 
        (format.mBitsPerChannel / 8);

    status = AudioUnitSetProperty(audio_unit,
                                kAudioUnitProperty_StreamFormat,
                                kAudioUnitScope_Input,
                                0,
                                &format,
                                sizeof(format));
    if (status != noErr) return status;

    // Set up render callback
    AURenderCallbackStruct callback = {0};
    callback.inputProc = audio_render_callback;
    callback.inputProcRefCon = NULL;

    status = AudioUnitSetProperty(audio_unit,
                                kAudioUnitProperty_SetRenderCallback,
                                kAudioUnitScope_Input,
                                0,
                                &callback,
                                sizeof(callback));
    if (status != noErr) return status;

    // Initialize and start audio unit
    status = AudioUnitInitialize(audio_unit);
    if (status != noErr) return status;

    return AudioOutputUnitStart(audio_unit);
}

OSStatus audio_input_init(void) {
    // Describe audio component
    AudioComponentDescription desc = {0};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    // Get default input component
    input_component = AudioComponentFindNext(NULL, &desc);
    if (!input_component) return -1;

    // Create audio unit instance
    OSStatus status = AudioComponentInstanceNew(input_component, &input_unit);
    if (status != noErr) return status;

    // Enable input and disable output
    UInt32 enable = 1;
    status = AudioUnitSetProperty(input_unit,
                                kAudioOutputUnitProperty_EnableIO,
                                kAudioUnitScope_Input,
                                1,
                                &enable,
                                sizeof(enable));
    if (status != noErr) return status;

    enable = 0;
    status = AudioUnitSetProperty(input_unit,
                                kAudioOutputUnitProperty_EnableIO,
                                kAudioUnitScope_Output,
                                0,
                                &enable,
                                sizeof(enable));
    if (status != noErr) return status;

    // Set up input callback
    AURenderCallbackStruct callback = {0};
    callback.inputProc = audio_input_callback;
    callback.inputProcRefCon = NULL;

    status = AudioUnitSetProperty(input_unit,
                                kAudioOutputUnitProperty_SetInputCallback,
                                kAudioUnitScope_Global,
                                0,
                                &callback,
                                sizeof(callback));
    if (status != noErr) return status;

    // Initialize and start audio unit
    status = AudioUnitInitialize(input_unit);
    if (status != noErr) return status;

    return AudioOutputUnitStart(input_unit);
}

int audio_buffer_init(void) {
    // Initialize output buffer
    g_audio_buffer.capacity = AUDIO_BUFFER_SIZE;
    g_audio_buffer.data = (int16_t*)calloc(g_audio_buffer.capacity, sizeof(int16_t));
    if (!g_audio_buffer.data) return -1;
    g_audio_buffer.size = 0;
    pthread_mutex_init(&g_audio_buffer.mutex, NULL);

    // Initialize input buffer
    g_input_buffer.capacity = AUDIO_BUFFER_SIZE;
    g_input_buffer.data = (int16_t*)calloc(g_input_buffer.capacity, sizeof(int16_t));
    if (!g_input_buffer.data) {
        free(g_audio_buffer.data);
        return -1;
    }
    g_input_buffer.size = 0;
    pthread_mutex_init(&g_input_buffer.mutex, NULL);

    return 0;
}

void audio_process_input(const int16_t* audio_data, int num_samples, int num_channels) {
    // Convert endianness and copy to buffer
    int16_t* converted_buffer = (int16_t*)malloc(num_samples * num_channels * sizeof(int16_t));
    if (converted_buffer) {
        for (int i = 0; i < num_samples * num_channels; i++) {
            converted_buffer[i] = OSSwapLittleToHostInt16(audio_data[i]);
        }
        audio_buffer_add(converted_buffer, num_samples, num_channels);
        free(converted_buffer);
    }
}

void audio_buffer_add(const int16_t* data, size_t samples, int channels) {
    pthread_mutex_lock(&g_audio_buffer.mutex);
    
    size_t total_samples = samples * channels;
    if (g_audio_buffer.size + total_samples > g_audio_buffer.capacity) {
        // Buffer full, remove oldest data
        memmove(g_audio_buffer.data, 
                g_audio_buffer.data + total_samples,
                (g_audio_buffer.size - total_samples) * sizeof(int16_t));
        g_audio_buffer.size -= total_samples;
    }
    
    // Add new data
    memcpy(g_audio_buffer.data + g_audio_buffer.size,
           data,
           total_samples * sizeof(int16_t));
    g_audio_buffer.size += total_samples;
    
    pthread_mutex_unlock(&g_audio_buffer.mutex);
}

void audio_cleanup(void) {
    if (audio_unit) {
        AudioOutputUnitStop(audio_unit);
        AudioUnitUninitialize(audio_unit);
        AudioComponentInstanceDispose(audio_unit);
        audio_unit = NULL;
    }

    if (input_unit) {
        AudioOutputUnitStop(input_unit);
        AudioUnitUninitialize(input_unit);
        AudioComponentInstanceDispose(input_unit);
        input_unit = NULL;
    }

    free(g_audio_buffer.data);
    free(g_input_buffer.data);
    pthread_mutex_destroy(&g_audio_buffer.mutex);
    pthread_mutex_destroy(&g_input_buffer.mutex);
} 