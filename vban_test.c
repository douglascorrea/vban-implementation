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
} vban_context_t;

// Audio Unit globals
static AudioComponentInstance audio_unit = NULL;
static AudioComponent output_component = NULL;

// Audio buffer structure
typedef struct {
    int16_t* data;
    size_t size;
    size_t capacity;
    pthread_mutex_t mutex;
} audio_buffer_t;

static audio_buffer_t g_audio_buffer = {0};

// Function prototypes
void* receive_thread(void* arg);
void process_audio(const int16_t* audio_data, int num_samples, int num_channels);
static OSStatus audio_render_callback(void *inRefCon,
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

    // Start receive thread
    if (pthread_create(&ctx->receive_thread, NULL, receive_thread, ctx) != 0) {
        perror("Failed to create receive thread");
        close(ctx->socket);
        free(ctx);
        return NULL;
    }

    return ctx;
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

// Modified process_audio function
void process_audio(const int16_t* audio_data, int num_samples, int num_channels) {
    static uint32_t packet_count = 0;
    static uint32_t total_samples = 0;
    
    // Print debug info every 100 packets
    if (++packet_count % 100 == 0) {
        printf("\nAudio Stats:\n");
        printf("Packets received: %u\n", packet_count);
        printf("Total samples: %u\n", total_samples);
        printf("Buffer size: %zu samples\n", g_audio_buffer.size);
        printf("Sample values: [%d, %d, %d, %d]\n", 
               OSSwapLittleToHostInt16(audio_data[0]),
               OSSwapLittleToHostInt16(audio_data[1]),
               OSSwapLittleToHostInt16(audio_data[2]),
               OSSwapLittleToHostInt16(audio_data[3]));
    }
    
    total_samples += num_samples;

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

// Cleanup VBAN context
void vban_cleanup(vban_context_t* ctx) {
    if (ctx) {
        ctx->is_running = 0;
        pthread_join(ctx->receive_thread, NULL);
        close(ctx->socket);
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

    // Initialize VBAN
    vban_context_t* ctx = vban_init(argv[1], argv[2]);
    if (!ctx) {
        printf("Failed to initialize VBAN\n");
        return 1;
    }

    printf("VBAN initialized. Streaming from %s with stream name %s\n", argv[1], argv[2]);
    printf("Audio output initialized. Playing to default output device.\n");

    // Wait for Ctrl+C
    while (1) {
        sleep(1);
    }

    // Cleanup
    vban_cleanup(ctx);
    return 0;
}