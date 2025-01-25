#ifndef VBAN4MAC_NETWORK_H
#define VBAN4MAC_NETWORK_H

#include <netinet/in.h>
#include <pthread.h>
#include "../include/vban4mac/types.h"

// Internal VBAN context structure
typedef struct vban_context_t {
    int socket;
    struct sockaddr_in remote_addr;
    char streamname[16];
    uint32_t frame_counter;
    int is_running;
    pthread_t receive_thread;
    pthread_t send_thread;
} vban_context_t;

// Network thread functions
void* network_receive_thread(void* arg);
void* network_send_thread(void* arg);

// Network initialization
int network_init(vban_context_t* ctx, const char* remote_ip);

// Network cleanup
void network_cleanup(vban_context_t* ctx);

#endif /* VBAN4MAC_NETWORK_H */ 