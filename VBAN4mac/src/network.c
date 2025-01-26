#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <libkern/OSByteOrder.h>
#include "../include/vban4mac/vban.h"
#include "network.h"
#include "audio.h"
#include "../include/vban4mac/types.h"

// Global audio buffers
extern audio_buffer_t g_input_buffer;

int network_init(vban_context_t* ctx, const char* remote_ip) {
    // Create UDP socket
    ctx->socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->socket < 0) {
        perror("Failed to create socket");
        return -1;
    }

    // Configure remote address (VoiceMeeter)
    ctx->remote_addr.sin_family = AF_INET;
    ctx->remote_addr.sin_port = htons(VBAN_DEFAULT_PORT);
    if (inet_pton(AF_INET, remote_ip, &ctx->remote_addr.sin_addr) != 1) {
        perror("Failed to set remote IP");
        close(ctx->socket);
        return -1;
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
        return -1;
    }

    if (bind(ctx->socket, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        perror("Failed to bind socket");
        close(ctx->socket);
        return -1;
    }

    return 0;
}

void* network_receive_thread(void* arg) {
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
                audio_process_input(audio_data, num_samples, num_channels);
            }
        }
    }

    return NULL;
}

void* network_send_thread(void* arg) {
    vban_context_t* ctx = (vban_context_t*)arg;
    const int samples_per_packet = 256;  // VBAN standard packet size
    int16_t send_buffer[samples_per_packet * 2];  // 2 channels
    uint64_t total_samples_sent = 0;
    uint32_t packets_sent = 0;

    printf("VBAN Send Thread Started\n");

    while (ctx->is_running) {
        pthread_mutex_lock(&g_input_buffer.mutex);
        
        // Check if we have enough data to send
        if (g_input_buffer.size >= samples_per_packet * 2) {  // 2 channels
            // Copy data to send buffer
            memcpy(send_buffer, g_input_buffer.data, samples_per_packet * 2 * sizeof(int16_t));
            
            // Remove the data we're about to send
            memmove(g_input_buffer.data, 
                   g_input_buffer.data + samples_per_packet * 2,
                   (g_input_buffer.size - samples_per_packet * 2) * sizeof(int16_t));
            g_input_buffer.size -= samples_per_packet * 2;
            
            pthread_mutex_unlock(&g_input_buffer.mutex);
            
            // Send the audio data
            if (vban_send_audio((vban_handle_t)ctx, send_buffer, samples_per_packet, 2) == 0) {
                packets_sent++;
                total_samples_sent += samples_per_packet;
                
                // Print stats every 100 packets
                if (packets_sent % 100 == 0) {
                    printf("VBAN Send Stats:\n");
                    printf("- Packets sent: %u\n", packets_sent);
                    printf("- Total samples sent: %llu\n", total_samples_sent);
                    printf("- Current input buffer size: %zu\n", g_input_buffer.size);
                }
            }
        } else {
            pthread_mutex_unlock(&g_input_buffer.mutex);
            // Sleep a bit if we don't have enough data
            usleep(1000);  // 1ms
        }
    }

    printf("VBAN Send Thread Stopped\n");
    return NULL;
}

void network_cleanup(vban_context_t* ctx) {
    if (ctx && ctx->socket >= 0) {
        close(ctx->socket);
        ctx->socket = -1;
    }
} 