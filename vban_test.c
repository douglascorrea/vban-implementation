/* VBAN Protocol Implementation for macOS (Apple Silicon)
 * Compatible with VoiceMeeter
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <libkern/OSByteOrder.h>

#define VBAN_HEADER_SIZE 28
#define VBAN_MAX_PACKET_SIZE 1436
#define VBAN_PROTOCOL_AUDIO 0x00
#define VBAN_DATATYPE_INT16 0x01
#define VBAN_DEFAULT_PORT 6980
#define VBAN_SAMPLE_RATE 48000
#define VBAN_SAMPLE_RATE_INDEX 3  // Index for 48kHz
#define VBAN_PROTOCOL_MAXNBS 256  // Maximum number of samples per packet
#define AUDIO_BUFFER_SIZE (VBAN_PROTOCOL_MAXNBS * 16)  // Buffer for ~256ms of audio

// VBAN Packet Header Structure
typedef struct __attribute__((packed)) {
    uint32_t vban;           // Contains 'VBAN' fourcc
    uint8_t format_SR;       // Sample rate index and sub protocol
    uint8_t format_nbs;      // Number of samples per frame (1-256)
    uint8_t format_nbc;      // Number of channels (1-256)
    uint8_t format_bit;      // Bit resolution and codec
    char streamname[16];     // Stream identifier
    uint32_t nuFrame;        // Frame counter
} vban_header_t;

// VBAN Context Structure
typedef struct {
    int socket;
    struct sockaddr_in remote_addr;
    char streamname[16];
    uint32_t frame_counter;
    int is_running;
    pthread_t receive_thread;
    pthread_t send_thread;  // New thread for sending microphone data
} vban_context_t;

// Audio Unit globals
static AudioComponentInstance audio_unit = NULL;
static AudioComponent output_component = NULL;
static AudioComponentInstance input_unit = NULL;
static AudioComponent input_component = NULL;

// Audio buffer structure
typedef struct {
    int16_t* data;
    size_t size;
    size_t capacity;
    pthread_mutex_t mutex;
} audio_buffer_t;

static audio_buffer_t g_audio_buffer = {0};
static audio_buffer_t g_input_buffer = {0};  // Buffer for microphone input

// Function prototypes
void* receive_thread(void* arg);
void* send_thread(void* arg);  // New thread for sending microphone data
void process_audio(const int16_t* audio_data, int num_samples, int num_channels);
int vban_send_audio(vban_context_t* ctx, const int16_t* audio_data, 
                   int num_samples, int num_channels);
static OSStatus audio_render_callback(void *inRefCon,
                                    AudioUnitRenderActionFlags *ioActionFlags,
                                    const AudioTimeStamp *inTimeStamp,
                                    UInt32 inBusNumber,
                                    UInt32 inNumberFrames,
                                    AudioBufferList *ioData);
static OSStatus audio_input_callback(void *inRefCon,
                                   AudioUnitRenderActionFlags *ioActionFlags,
                                   const AudioTimeStamp *inTimeStamp,
                                   UInt32 inBusNumber,
                                   UInt32 inNumberFrames,
                                   AudioBufferList *ioData);

// Sample rate list as per VBAN spec
static const long VBAN_SR_LIST[] = {
    6000, 12000, 24000, 48000, 96000, 192000, 384000,
    8000, 16000, 32000, 64000, 128000, 256000, 512000,
    11025, 22050, 44100, 88200, 176400, 352800, 705600
};

// Modified audio output initialization
static OSStatus init_audio_output() {
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
    if (status != noErr) {
        printf("Failed to set stream format: %d\n", (int)status);
        return status;
    }

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
    if (status != noErr) {
        printf("Failed to set render callback\n");
        return status;
    }

    // Initialize and start audio unit
    status = AudioUnitInitialize(audio_unit);
    if (status != noErr) {
        printf("Failed to initialize audio unit\n");
        return status;
    }

    status = AudioOutputUnitStart(audio_unit);
    if (status != noErr) {
        printf("Failed to start audio unit\n");
        return status;
    }

    return noErr;
}

// Initialize audio input (microphone)
static OSStatus init_audio_input() {
    OSStatus status;
    AudioStreamBasicDescription deviceFormat;
    
    // Print available audio devices
    AudioObjectPropertyAddress propertyAddress = {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    AudioDeviceID inputDevice;
    UInt32 propertySize = sizeof(AudioDeviceID);
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                      &propertyAddress,
                                      0,
                                      NULL,
                                      &propertySize,
                                      &inputDevice);
    
    if (status == noErr) {
        // Get the device name
        propertyAddress.mSelector = kAudioDevicePropertyDeviceNameCFString;
        propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
        CFStringRef deviceName;
        propertySize = sizeof(CFStringRef);
        status = AudioObjectGetPropertyData(inputDevice,
                                          &propertyAddress,
                                          0,
                                          NULL,
                                          &propertySize,
                                          &deviceName);
        
        if (status == noErr) {
            char name[256];
            CFStringGetCString(deviceName, name, sizeof(name), kCFStringEncodingUTF8);
            printf("Default input device: %s\n", name);
            CFRelease(deviceName);

            // Get the device's current format
            propertyAddress.mSelector = kAudioDevicePropertyStreamFormat;
            propertyAddress.mScope = kAudioObjectPropertyScopeInput;
            propertySize = sizeof(AudioStreamBasicDescription);
            status = AudioObjectGetPropertyData(inputDevice,
                                              &propertyAddress,
                                              0,
                                              NULL,
                                              &propertySize,
                                              &deviceFormat);
            if (status == noErr) {
                printf("Input device format:\n");
                printf("- Sample rate: %.0f Hz\n", deviceFormat.mSampleRate);
                printf("- Channels: %u\n", deviceFormat.mChannelsPerFrame);
                printf("- Bits per channel: %u\n", deviceFormat.mBitsPerChannel);
                printf("- Format flags: 0x%x\n", (unsigned int)deviceFormat.mFormatFlags);
            } else {
                printf("Failed to get input device format: %d\n", (int)status);
            }
        } else {
            printf("Failed to get input device name: %d\n", (int)status);
        }
    } else {
        printf("Failed to get default input device: %d\n", (int)status);
        return status;
    }

    // Describe audio component
    AudioComponentDescription desc = {0};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;  // Use HAL for input
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    // Get default input component
    input_component = AudioComponentFindNext(NULL, &desc);
    if (!input_component) {
        printf("Failed to find default input component\n");
        return -1;
    }

    // Create audio unit instance
    status = AudioComponentInstanceNew(input_component, &input_unit);
    if (status != noErr) {
        printf("Failed to create input unit instance\n");
        return status;
    }

    // First, enable input and disable output
    UInt32 enable = 1;
    status = AudioUnitSetProperty(input_unit,
                                kAudioOutputUnitProperty_EnableIO,
                                kAudioUnitScope_Input,
                                1,  // Input bus
                                &enable,
                                sizeof(enable));
    if (status != noErr) {
        printf("Failed to enable input: %d\n", (int)status);
        return status;
    }

    enable = 0;
    status = AudioUnitSetProperty(input_unit,
                                kAudioOutputUnitProperty_EnableIO,
                                kAudioUnitScope_Output,
                                0,  // Output bus
                                &enable,
                                sizeof(enable));
    if (status != noErr) {
        printf("Failed to disable output: %d\n", (int)status);
        return status;
    }

    // Second, set the stream format
    status = AudioUnitSetProperty(input_unit,
                                kAudioUnitProperty_StreamFormat,
                                kAudioUnitScope_Output,
                                1,  // Input bus
                                &deviceFormat,
                                sizeof(deviceFormat));
    if (status != noErr) {
        printf("Failed to set input stream format: %d\n", (int)status);
        return status;
    }

    printf("Successfully set input stream format\n");

    // Third, now we can set the device
    status = AudioUnitSetProperty(input_unit,
                                kAudioOutputUnitProperty_CurrentDevice,
                                kAudioUnitScope_Global,
                                0,
                                &inputDevice,
                                sizeof(inputDevice));
    if (status != noErr) {
        printf("Failed to set input device: %d\n", (int)status);
        return status;
    }

    printf("Successfully set input device\n");

    // Finally, set up input callback
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
        printf("Failed to set input callback\n");
        return status;
    }

    // Initialize and start audio unit
    status = AudioUnitInitialize(input_unit);
    if (status != noErr) {
        printf("Failed to initialize input unit\n");
        return status;
    }

    status = AudioOutputUnitStart(input_unit);
    if (status != noErr) {
        printf("Failed to start input unit\n");
        return status;
    }

    printf("Successfully started input unit\n");
    return noErr;
}

// Initialize audio buffer
static int init_audio_buffer() {
    g_audio_buffer.capacity = AUDIO_BUFFER_SIZE;
    g_audio_buffer.data = (int16_t*)calloc(g_audio_buffer.capacity, sizeof(int16_t));
    if (!g_audio_buffer.data) return -1;
    
    g_audio_buffer.size = 0;
    pthread_mutex_init(&g_audio_buffer.mutex, NULL);
    return 0;
}

// Add audio data to buffer
static void add_to_audio_buffer(const int16_t* data, size_t samples, int channels) {
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

// Modified audio render callback
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

// Modified process_audio function
void process_audio(const int16_t* audio_data, int num_samples, int num_channels) {
    // Convert endianness and copy to buffer
    int16_t* converted_buffer = (int16_t*)malloc(num_samples * num_channels * sizeof(int16_t));
    if (converted_buffer) {
        for (int i = 0; i < num_samples * num_channels; i++) {
            converted_buffer[i] = OSSwapLittleToHostInt16(audio_data[i]);
        }
        add_to_audio_buffer(converted_buffer, num_samples, num_channels);
        free(converted_buffer);
    }
}

// Audio input callback
static OSStatus audio_input_callback(void *inRefCon,
                                   AudioUnitRenderActionFlags *ioActionFlags,
                                   const AudioTimeStamp *inTimeStamp,
                                   UInt32 inBusNumber,
                                   UInt32 inNumberFrames,
                                   AudioBufferList *ioData) {
    static uint32_t callback_count = 0;
    static uint64_t total_samples = 0;
    
    // Create buffer list for rendered audio
    AudioBufferList buffer_list;
    buffer_list.mNumberBuffers = 1;
    buffer_list.mBuffers[0].mNumberChannels = 1;  // Mono input
    buffer_list.mBuffers[0].mDataByteSize = inNumberFrames * sizeof(float);  // 32-bit float
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
            total_samples += output_samples;
            
            // Print debug info every 100 callbacks
            if (++callback_count % 100 == 0) {
                printf("\nMicrophone Input Stats:\n");
                printf("- Callbacks: %u\n", callback_count);
                printf("- Total samples captured: %llu\n", total_samples);
                printf("- Current buffer size: %zu samples\n", g_input_buffer.size);
                printf("- Frame size: %u samples (%u bytes)\n", 
                       inNumberFrames, buffer_list.mBuffers[0].mDataByteSize);
                
                // Print max amplitude of original float data
                float max_sample = 0.0f;
                for (size_t i = 0; i < inNumberFrames; i++) {
                    float abs_sample = fabsf(input_samples[i]);
                    if (abs_sample > max_sample) {
                        max_sample = abs_sample;
                    }
                }
                printf("- Max sample amplitude: %.6f\n", max_sample);
            }
        } else {
            printf("Input buffer full, dropping %zu samples\n", output_samples);
        }
        
        pthread_mutex_unlock(&g_input_buffer.mutex);
    } else {
        printf("AudioUnitRender failed with status: %d\n", (int)status);
    }

    free(buffer_list.mBuffers[0].mData);
    return status;
}

// Send audio data to VoiceMeeter
int vban_send_audio(vban_context_t* ctx, const int16_t* audio_data, 
                   int num_samples, int num_channels) {
    if (!ctx || !audio_data || num_samples <= 0 || num_channels <= 0) return -1;

    // Prepare packet
    uint8_t packet[VBAN_HEADER_SIZE + VBAN_MAX_PACKET_SIZE];
    vban_header_t* header = (vban_header_t*)packet;

    // Fill header
    header->vban = htonl(('V' << 24) | ('B' << 16) | ('A' << 8) | 'N');
    header->format_SR = VBAN_SAMPLE_RATE_INDEX;
    header->format_nbs = num_samples - 1;
    header->format_nbc = num_channels - 1;
    header->format_bit = VBAN_DATATYPE_INT16;
    strncpy(header->streamname, ctx->streamname, 16);
    header->nuFrame = ctx->frame_counter++;

    // Copy audio data
    size_t data_size = num_samples * num_channels * sizeof(int16_t);
    if (data_size > VBAN_MAX_PACKET_SIZE) return -2;
    memcpy(packet + VBAN_HEADER_SIZE, audio_data, data_size);

    // Send packet
    ssize_t sent = sendto(ctx->socket, packet, VBAN_HEADER_SIZE + data_size, 0,
                         (struct sockaddr*)&ctx->remote_addr, sizeof(ctx->remote_addr));
    
    return (sent == VBAN_HEADER_SIZE + data_size) ? 0 : -3;
}

// Send thread function for microphone data
void* send_thread(void* arg) {
    vban_context_t* ctx = (vban_context_t*)arg;
    const int samples_per_packet = 256;  // VBAN standard packet size
    int16_t send_buffer[samples_per_packet * 2];  // 2 channels
    uint32_t packets_sent = 0;
    uint64_t total_samples_sent = 0;

    printf("\nVBAN Send Thread Started\n");

    while (ctx->is_running) {
        pthread_mutex_lock(&g_input_buffer.mutex);
        
        if (g_input_buffer.size >= samples_per_packet * 2) {
            // Copy data to send buffer
            memcpy(send_buffer, g_input_buffer.data, samples_per_packet * 2 * sizeof(int16_t));
            
            // Remove the data we're about to send
            memmove(g_input_buffer.data,
                    g_input_buffer.data + samples_per_packet * 2,
                    (g_input_buffer.size - samples_per_packet * 2) * sizeof(int16_t));
            g_input_buffer.size -= samples_per_packet * 2;
            
            pthread_mutex_unlock(&g_input_buffer.mutex);

            // Convert endianness for network transmission
            for (int i = 0; i < samples_per_packet * 2; i++) {
                send_buffer[i] = OSSwapHostToLittleInt16(send_buffer[i]);
            }

            // Send the audio data
            int result = vban_send_audio(ctx, send_buffer, samples_per_packet, 2);
            total_samples_sent += samples_per_packet * 2;
            
            // Print debug info every 100 packets
            if (++packets_sent % 100 == 0) {
                printf("\nVBAN Send Stats:\n");
                printf("- Packets sent: %u\n", packets_sent);
                printf("- Total samples sent: %llu\n", total_samples_sent);
                printf("- Last send result: %d\n", result);
                printf("- Current input buffer size: %zu\n", g_input_buffer.size);
            }
        } else {
            pthread_mutex_unlock(&g_input_buffer.mutex);
            usleep(1000);  // Sleep for 1ms if not enough data
        }
    }

    printf("\nVBAN Send Thread Stopped\n");
    return NULL;
}

// Initialize VBAN context
vban_context_t* vban_init(const char* remote_ip, const char* streamname) {
    vban_context_t* ctx = calloc(1, sizeof(vban_context_t));
    if (!ctx) {
        perror("Failed to allocate context");
        return NULL;
    }

    // Create UDP socket
    ctx->socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->socket < 0) {
        perror("Failed to create socket");
        free(ctx);
        return NULL;
    }

    // Configure remote address (VoiceMeeter)
    ctx->remote_addr.sin_family = AF_INET;
    ctx->remote_addr.sin_port = htons(VBAN_DEFAULT_PORT);
    if (inet_pton(AF_INET, remote_ip, &ctx->remote_addr.sin_addr) != 1) {
        perror("Failed to set remote IP");
        close(ctx->socket);
        free(ctx);
        return NULL;
    }

    // Configure local address for receiving
    struct sockaddr_in local_addr = {0};
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(VBAN_DEFAULT_PORT);
    local_addr.sin_addr.s_addr = INADDR_ANY;

    // Add socket option to reuse address
    int reuse = 1;
    if (setsockopt(ctx->socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("Failed to set socket options");
        close(ctx->socket);
        free(ctx);
        return NULL;
    }

    if (bind(ctx->socket, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        perror("Failed to bind socket");
        close(ctx->socket);
        free(ctx);
        return NULL;
    }

    strncpy(ctx->streamname, streamname, sizeof(ctx->streamname) - 1);
    ctx->frame_counter = 0;
    ctx->is_running = 1;

    // Initialize input buffer
    g_input_buffer.capacity = AUDIO_BUFFER_SIZE;
    g_input_buffer.data = (int16_t*)calloc(g_input_buffer.capacity, sizeof(int16_t));
    if (!g_input_buffer.data) {
        perror("Failed to allocate input buffer");
        close(ctx->socket);
        free(ctx);
        return NULL;
    }
    g_input_buffer.size = 0;
    pthread_mutex_init(&g_input_buffer.mutex, NULL);

    // Start receive thread
    if (pthread_create(&ctx->receive_thread, NULL, receive_thread, ctx) != 0) {
        perror("Failed to create receive thread");
        close(ctx->socket);
        free(g_input_buffer.data);
        free(ctx);
        return NULL;
    }

    // Start send thread
    if (pthread_create(&ctx->send_thread, NULL, send_thread, ctx) != 0) {
        perror("Failed to create send thread");
        ctx->is_running = 0;
        pthread_join(ctx->receive_thread, NULL);
        close(ctx->socket);
        free(g_input_buffer.data);
        free(ctx);
        return NULL;
    }

    return ctx;
}

// Receive thread function
void* receive_thread(void* arg) {
    vban_context_t* ctx = (vban_context_t*)arg;
    uint8_t packet[VBAN_HEADER_SIZE + VBAN_MAX_PACKET_SIZE];

    while (ctx->is_running) {
        struct sockaddr_in sender_addr;
        socklen_t sender_len = sizeof(sender_addr);

        ssize_t received = recvfrom(ctx->socket, packet, sizeof(packet), 0,
                                  (struct sockaddr*)&sender_addr, &sender_len);

        if (received > VBAN_HEADER_SIZE) {
            vban_header_t* header = (vban_header_t*)packet;
            
            // Validate VBAN packet
            if (ntohl(header->vban) == (('V' << 24) | ('B' << 16) | ('A' << 8) | 'N')) {
                int num_samples = (header->format_nbs + 1);
                int num_channels = (header->format_nbc + 1);
                
                // Process received audio data
                const int16_t* audio_data = (const int16_t*)(packet + VBAN_HEADER_SIZE);
                process_audio(audio_data, num_samples, num_channels);
            }
        }
    }

    return NULL;
}

// Cleanup VBAN context
void vban_cleanup(vban_context_t* ctx) {
    if (ctx) {
        ctx->is_running = 0;
        pthread_join(ctx->receive_thread, NULL);
        pthread_join(ctx->send_thread, NULL);
        close(ctx->socket);
        free(g_input_buffer.data);
        free(ctx);
    }
}

// Modified main function
int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("Usage: %s <remote_ip> <stream_name>\n", argv[0]);
        return 1;
    }

    // Initialize audio system
    if (init_audio_buffer() != 0) {
        printf("Failed to initialize audio buffer\n");
        return 1;
    }

    if (init_audio_output() != noErr) {
        printf("Failed to initialize audio output\n");
        return 1;
    }

    if (init_audio_input() != noErr) {
        printf("Failed to initialize audio input\n");
        return 1;
    }

    // Initialize VBAN
    vban_context_t* ctx = vban_init(argv[1], argv[2]);
    if (!ctx) {
        printf("Failed to initialize VBAN\n");
        return 1;
    }

    printf("VBAN initialized successfully:\n");
    printf("- Receiving from %s with stream name %s\n", argv[1], argv[2]);
    printf("- Sending microphone input to %s with stream name %s\n", argv[1], argv[2]);
    printf("- Audio output initialized and playing to default output device\n");
    printf("- Audio input initialized and capturing from default input device\n");
    printf("\nPress Ctrl+C to stop...\n");

    // Wait for Ctrl+C
    while (1) {
        sleep(1);
    }

    // Cleanup
    vban_cleanup(ctx);
    return 0;
}