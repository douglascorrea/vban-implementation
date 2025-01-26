#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "../include/vban4mac/vban.h"
#include "../src/audio.h"

static volatile int running = 1;

void signal_handler(int signum) {
    printf("\nSignal %d received, stopping...\n", signum);
    running = 0;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <remote_ip> <stream_name>\n", argv[0]);
        return 1;
    }

    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // List all available audio devices
    audio_list_devices();

    // Get user input for device selection
    AudioDeviceID inputDeviceID, outputDeviceID;
    char input[256];

    printf("Enter the Device ID for input (or press Enter for default): ");
    fgets(input, sizeof(input), stdin);
    if (input[0] != '\n') {
        inputDeviceID = (AudioDeviceID)atoi(input);
    }

    printf("Enter the Device ID for output (or press Enter for default): ");
    fgets(input, sizeof(input), stdin);
    if (input[0] != '\n') {
        outputDeviceID = (AudioDeviceID)atoi(input);
    }

    // Initialize VBAN
    struct vban_context_t* ctx = vban_init(argv[1], argv[2]);
    if (!ctx) {
        printf("Failed to initialize VBAN\n");
        return 1;
    }

    // Set selected devices if specified
    if (input[0] != '\n') {
        if (audio_set_input_device(inputDeviceID) != noErr) {
            printf("Failed to set input device\n");
            vban_cleanup(ctx);
            return 1;
        }
    }

    if (input[0] != '\n') {
        if (audio_set_output_device(outputDeviceID) != noErr) {
            printf("Failed to set output device\n");
            vban_cleanup(ctx);
            return 1;
        }
    }

    printf("VBAN bridge initialized with:\n");
    printf("Remote IP: %s\n", argv[1]);
    printf("Stream name: %s\n", argv[2]);
    printf("\nPress Ctrl+C to stop\n\n");

    // Wait for signal
    while (running && vban_is_running(ctx)) {
        sleep(1);
    }

    // Cleanup
    vban_cleanup(ctx);
    return 0;
} 