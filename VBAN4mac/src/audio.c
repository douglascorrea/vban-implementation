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
audio_buffer_t g_audio_buffer = {0};
audio_buffer_t g_input_buffer = {0};

// Monitoring callbacks
static audio_monitor_callback input_monitor = NULL;
static audio_monitor_callback output_monitor = NULL;

void audio_set_input_monitor(audio_monitor_callback callback) {
    input_monitor = callback;
}

void audio_set_output_monitor(audio_monitor_callback callback) {
    output_monitor = callback;
}

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
        
        // Call output monitor if set
        if (output_monitor) {
            float* monitor_buffer = malloc(frames_to_copy * sizeof(float));
            if (monitor_buffer) {
                // Convert int16 to float for monitoring
                for (size_t i = 0; i < frames_to_copy; i++) {
                    monitor_buffer[i] = left[i] / 32767.0f;  // Use left channel for monitoring
                }
                output_monitor(monitor_buffer, frames_to_copy);
                free(monitor_buffer);
            }
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
    buffer_list.mBuffers[0].mDataByteSize = inNumberFrames * sizeof(float);  // Device uses 32-bit float
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
        
        // Call input monitor if set
        if (input_monitor) {
            input_monitor(buffer_list.mBuffers[0].mData, inNumberFrames);
        }
        
        // Convert float mono to int16 stereo
        float* input_samples = (float*)buffer_list.mBuffers[0].mData;
        size_t output_samples = inNumberFrames * 2; // Stereo output
        
        if (g_input_buffer.size + output_samples <= g_input_buffer.capacity) {
            // Convert float mono to int16 stereo
            for (size_t i = 0; i < inNumberFrames; i++) {
                // Convert float to int16 and duplicate to both channels
                int16_t sample = (int16_t)(input_samples[i] * 32767.0f);
                g_input_buffer.data[g_input_buffer.size + i * 2] = sample;
                g_input_buffer.data[g_input_buffer.size + i * 2 + 1] = sample;
            }
            
            g_input_buffer.size += output_samples;
        }
        
        pthread_mutex_unlock(&g_input_buffer.mutex);
    } else {
        printf("AudioUnitRender failed with status: %d\n", (int)status);
    }

    free(buffer_list.mBuffers[0].mData);
    return status;
}

// Audio initialization functions
OSStatus audio_output_init(void) {
    // Describe audio component
    AudioComponentDescription desc = {0};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    // Get output component
    output_component = AudioComponentFindNext(NULL, &desc);
    if (!output_component) {
        printf("Failed to find output component\n");
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

    // Enable output on bus 0
    UInt32 enable = 1;
    status = AudioUnitSetProperty(audio_unit,
                                kAudioOutputUnitProperty_EnableIO,
                                kAudioUnitScope_Output,
                                0,
                                &enable,
                                sizeof(enable));
    if (status != noErr) {
        printf("Failed to enable output: %d\n", (int)status);
        return status;
    }

    // Initialize audio unit
    status = AudioUnitInitialize(audio_unit);
    if (status != noErr) return status;

    return AudioOutputUnitStart(audio_unit);
}

OSStatus audio_input_init(void) {
    printf("Starting audio input initialization...\n");
    
    // Describe audio component
    AudioComponentDescription desc = {0};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    // Get input component
    input_component = AudioComponentFindNext(NULL, &desc);
    if (!input_component) {
        printf("Failed to find input component\n");
        return -1;
    }
    printf("Found input component\n");

    // Create audio unit instance
    OSStatus status = AudioComponentInstanceNew(input_component, &input_unit);
    if (status != noErr) {
        printf("Failed to create input unit instance: %d\n", (int)status);
        return status;
    }
    printf("Created input unit instance\n");

    // Enable input on bus 1
    UInt32 enable = 1;
    status = AudioUnitSetProperty(input_unit,
                                kAudioOutputUnitProperty_EnableIO,
                                kAudioUnitScope_Input,
                                1,
                                &enable,
                                sizeof(enable));
    if (status != noErr) {
        printf("Failed to enable input: %d\n", (int)status);
        return status;
    }
    printf("Enabled input on bus 1\n");

    // Disable output on bus 0
    enable = 0;
    status = AudioUnitSetProperty(input_unit,
                                kAudioOutputUnitProperty_EnableIO,
                                kAudioUnitScope_Output,
                                0,
                                &enable,
                                sizeof(enable));
    if (status != noErr) {
        printf("Failed to disable output: %d\n", (int)status);
        return status;
    }
    printf("Disabled output on bus 0\n");

    // Set up stream format for input
    AudioStreamBasicDescription format = {0};
    format.mSampleRate = VBAN_SAMPLE_RATE;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | 
                         kAudioFormatFlagIsPacked;
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = 1; // Mono input
    format.mBitsPerChannel = 32;  // Float32
    format.mBytesPerPacket = format.mBytesPerFrame = 
        (format.mBitsPerChannel / 8) * format.mChannelsPerFrame;

    // Set format for input scope (recording)
    status = AudioUnitSetProperty(input_unit,
                                kAudioUnitProperty_StreamFormat,
                                kAudioUnitScope_Output,  // Output scope for input bus
                                1,                       // Input bus
                                &format,
                                sizeof(format));
    if (status != noErr) {
        printf("Failed to set input scope format: %d\n", (int)status);
        return status;
    }
    printf("Successfully set input scope format\n");

    // Set up input callback
    AURenderCallbackStruct callback = {0};
    callback.inputProc = audio_input_callback;
    callback.inputProcRefCon = NULL;

    status = AudioUnitSetProperty(input_unit,
                                kAudioOutputUnitProperty_SetInputCallback,
                                kAudioUnitScope_Global,
                                1,  // Input bus
                                &callback,
                                sizeof(callback));
    if (status != noErr) {
        printf("Failed to set input callback: %d\n", (int)status);
        return status;
    }
    printf("Input callback registered successfully\n");

    // Initialize audio unit
    status = AudioUnitInitialize(input_unit);
    if (status != noErr) {
        printf("Failed to initialize input unit: %d\n", (int)status);
        return status;
    }
    printf("Input unit initialized\n");

    return noErr;
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

// Function to get device name
char* get_device_name(AudioDeviceID deviceID) {
    AudioObjectPropertyAddress property = {
        kAudioDevicePropertyDeviceNameCFString,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMaster
    };
    
    CFStringRef deviceName;
    UInt32 size = sizeof(CFStringRef);
    OSStatus status = AudioObjectGetPropertyData(deviceID, &property, 0, NULL, &size, &deviceName);
    
    if (status != noErr) {
        return NULL;
    }
    
    char* name = malloc(256);
    if (!CFStringGetCString(deviceName, name, 256, kCFStringEncodingUTF8)) {
        free(name);
        CFRelease(deviceName);
        return NULL;
    }
    
    CFRelease(deviceName);
    return name;
}

// Function to get device sample rate
static Float64 get_device_sample_rate(AudioDeviceID deviceID, bool isInput) {
    AudioObjectPropertyAddress property = {
        kAudioDevicePropertyNominalSampleRate,
        isInput ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMaster
    };
    
    Float64 sampleRate;
    UInt32 size = sizeof(Float64);
    OSStatus status = AudioObjectGetPropertyData(deviceID, &property, 0, NULL, &size, &sampleRate);
    
    return (status == noErr) ? sampleRate : 0.0;
}

// Function to list all audio devices
void audio_list_devices(void) {
    // Get the number of audio devices
    AudioObjectPropertyAddress property = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMaster
    };
    
    UInt32 size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &property, 0, NULL, &size);
    if (status != noErr) {
        printf("Failed to get device list size\n");
        return;
    }
    
    // Get the device IDs
    int deviceCount = size / sizeof(AudioDeviceID);
    AudioDeviceID* devices = malloc(size);
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, 0, NULL, &size, devices);
    if (status != noErr) {
        printf("Failed to get device list\n");
        free(devices);
        return;
    }
    
    // Get default input and output devices
    AudioDeviceID defaultInput, defaultOutput;
    size = sizeof(AudioDeviceID);
    
    property.mSelector = kAudioHardwarePropertyDefaultInputDevice;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, 0, NULL, &size, &defaultInput);
    
    property.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, 0, NULL, &size, &defaultOutput);
    
    printf("\nAvailable Audio Devices:\n");
    printf("------------------------\n");
    
    for (int i = 0; i < deviceCount; i++) {
        AudioDeviceID deviceID = devices[i];
        char* name = get_device_name(deviceID);
        if (!name) continue;
        
        // Check if device has input/output capabilities
        property.mSelector = kAudioDevicePropertyStreamConfiguration;
        
        // Check input channels
        property.mScope = kAudioDevicePropertyScopeInput;
        status = AudioObjectGetPropertyDataSize(deviceID, &property, 0, NULL, &size);
        AudioBufferList* bufferList = malloc(size);
        status = AudioObjectGetPropertyData(deviceID, &property, 0, NULL, &size, bufferList);
        UInt32 inputChannels = 0;
        for (UInt32 i = 0; i < bufferList->mNumberBuffers; i++) {
            inputChannels += bufferList->mBuffers[i].mNumberChannels;
        }
        free(bufferList);
        
        // Check output channels
        property.mScope = kAudioDevicePropertyScopeOutput;
        status = AudioObjectGetPropertyDataSize(deviceID, &property, 0, NULL, &size);
        bufferList = malloc(size);
        status = AudioObjectGetPropertyData(deviceID, &property, 0, NULL, &size, bufferList);
        UInt32 outputChannels = 0;
        for (UInt32 i = 0; i < bufferList->mNumberBuffers; i++) {
            outputChannels += bufferList->mBuffers[i].mNumberChannels;
        }
        free(bufferList);
        
        // Get sample rates
        Float64 inputRate = get_device_sample_rate(deviceID, true);
        Float64 outputRate = get_device_sample_rate(deviceID, false);
        
        printf("\nDevice ID: %u - %s\n", (unsigned int)deviceID, name);
        if (deviceID == defaultInput) printf("  * Default Input Device *\n");
        if (deviceID == defaultOutput) printf("  * Default Output Device *\n");
        if (inputChannels > 0) {
            printf("  Input: %u channels @ %.0f Hz\n", inputChannels, inputRate);
        }
        if (outputChannels > 0) {
            printf("  Output: %u channels @ %.0f Hz\n", outputChannels, outputRate);
        }
        
        free(name);
    }
    
    free(devices);
    printf("\n");
}

// Function to set input device
OSStatus audio_set_input_device(AudioDeviceID deviceID) {
    if (!input_unit) {
        printf("Audio input unit not initialized\n");
        return -1;
    }
    
    OSStatus status = AudioUnitSetProperty(input_unit,
                                         kAudioOutputUnitProperty_CurrentDevice,
                                         kAudioUnitScope_Global,
                                         0,
                                         &deviceID,
                                         sizeof(deviceID));

    if (status != noErr) {
        printf("Failed to set input device: %d\n", (int)status);
        return status;
    }
    
    char* name = get_device_name(deviceID);
    if (name) {
        printf("Successfully set input device to: %s\n", name);
        free(name);
    }
    
    return noErr;
}

// Function to set output device
OSStatus audio_set_output_device(AudioDeviceID deviceID) {
    if (!audio_unit) {
        printf("Audio output unit not initialized\n");
        return -1;
    }
    
    OSStatus status = AudioUnitSetProperty(audio_unit,
                                         kAudioOutputUnitProperty_CurrentDevice,
                                         kAudioUnitScope_Global,
                                         0,
                                         &deviceID,
                                         sizeof(deviceID));
                                         
    if (status != noErr) {
        printf("Failed to set output device: %d\n", (int)status);
        return status;
    }
    
    char* name = get_device_name(deviceID);
    if (name) {
        printf("Successfully set output device to: %s\n", name);
        free(name);
    }
    
    return noErr;
}

OSStatus audio_start_input(void) {
    if (!input_unit) {
        printf("Audio input unit not initialized\n");
        return -1;
    }

    OSStatus status = AudioOutputUnitStart(input_unit);
    if (status != noErr) {
        printf("Failed to start input unit: %d\n", (int)status);
        return status;
    }
    printf("Input unit started successfully\n");
    return noErr;
} 