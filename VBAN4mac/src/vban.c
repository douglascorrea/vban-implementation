#include <string.h>
#include <stdlib.h>
#include "../include/vban4mac/vban.h"
#include "../include/vban4mac/types.h"
#include "network.h"
#include "audio.h"

vban_handle_t vban_init(const char* remote_ip, const char* stream_name) {
    // Allocate context
    vban_context_t* ctx = calloc(1, sizeof(vban_context_t));
    if (!ctx) {
        return NULL;
    }

    // Initialize network
    if (network_init(ctx, remote_ip) != 0) {
        free(ctx);
        return NULL;
    }

    // Copy stream name
    strncpy(ctx->streamname, stream_name, sizeof(ctx->streamname) - 1);
    ctx->frame_counter = 0;
    ctx->is_running = 1;

    // Initialize audio
    if (audio_buffer_init() != 0 ||
        audio_output_init() != noErr ||
        audio_input_init() != noErr) {
        network_cleanup(ctx);
        free(ctx);
        return NULL;
    }

    // Start network threads
    if (pthread_create(&ctx->receive_thread, NULL, network_receive_thread, ctx) != 0 ||
        pthread_create(&ctx->send_thread, NULL, network_send_thread, ctx) != 0) {
        ctx->is_running = 0;
        pthread_join(ctx->receive_thread, NULL);
        network_cleanup(ctx);
        audio_cleanup();
        free(ctx);
        return NULL;
    }

    return (vban_handle_t)ctx;
}

void vban_cleanup(vban_handle_t handle) {
    vban_context_t* ctx = (vban_context_t*)handle;
    if (ctx) {
        ctx->is_running = 0;
        pthread_join(ctx->receive_thread, NULL);
        pthread_join(ctx->send_thread, NULL);
        network_cleanup(ctx);
        audio_cleanup();
        free(ctx);
    }
}

int vban_send_audio(vban_handle_t handle, const int16_t* audio_data, 
                   int num_samples, int num_channels) {
    vban_context_t* ctx = (vban_context_t*)handle;
    if (!ctx || !audio_data || num_samples <= 0 || num_channels <= 0) {
        return -1;
    }

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
    if (data_size > VBAN_MAX_PACKET_SIZE) {
        return -2;
    }
    memcpy(packet + VBAN_HEADER_SIZE, audio_data, data_size);

    // Send packet
    ssize_t sent = sendto(ctx->socket, packet, VBAN_HEADER_SIZE + data_size, 0,
                         (struct sockaddr*)&ctx->remote_addr, sizeof(ctx->remote_addr));
    
    return (sent == VBAN_HEADER_SIZE + data_size) ? 0 : -3;
}

int vban_is_running(vban_handle_t handle) {
    vban_context_t* ctx = (vban_context_t*)handle;
    return ctx ? ctx->is_running : 0;
} 