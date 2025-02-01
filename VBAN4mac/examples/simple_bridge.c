#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <CoreAudio/CoreAudio.h>
#include <vban4mac/vban.h>
#include "../src/audio.h"

static volatile int running = 1;

static void handle_signal(int sig) {
    printf("\nSignal %d received, stopping...\n", sig);
    running = 0;
}

static AudioDeviceID get_user_device_selection(const char* type) {
    AudioDeviceID deviceID;
    char input[16];
    
    while (1) {
        printf("\nEnter %s device ID (or press Enter for system default): ", type);
        fflush(stdout);
        
        if (fgets(input, sizeof(input), stdin)) {
            // Remove newline
            input[strcspn(input, "\n")] = 0;
            
            // Empty input means use default
            if (strlen(input) == 0) {
                AudioObjectPropertyAddress property = {
                    type[0] == 'i' ? kAudioHardwarePropertyDefaultInputDevice : kAudioHardwarePropertyDefaultOutputDevice,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                
                UInt32 size = sizeof(AudioDeviceID);
                AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, 0, NULL, &size, &deviceID);
                printf("Using default %s device (ID: %u)\n", type, (unsigned int)deviceID);
                return deviceID;
            }
            
            // Try to parse device ID
            char* endptr;
            unsigned long id = strtoul(input, &endptr, 10);
            if (*endptr == '\0') {
                deviceID = (AudioDeviceID)id;
                char* name = get_device_name(deviceID);
                if (name) {
                    printf("Selected device: %s\n", name);
                    free(name);
                    return deviceID;
                }
            }
        }
        printf("Invalid device ID. Please try again.\n");
    }
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("Usage: %s <remote_ip> <stream_name>\n", argv[0]);
        return 1;
    }

    // Set up signal handling
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // List available audio devices
    printf("\nAvailable Audio Devices:\n");
    audio_list_devices();

    // Get user selected devices
    AudioDeviceID inputDevice = get_user_device_selection("input");
    AudioDeviceID outputDevice = get_user_device_selection("output");

    // Initialize VBAN
    vban_handle_t vban = vban_init(argv[1], argv[2]);
    if (!vban) {
        printf("Failed to initialize VBAN\n");
        return 1;
    }

    // Set input and output devices
    if (audio_set_input_device(inputDevice) != noErr) {
        printf("Failed to set input device\n");
        vban_cleanup(vban);
        return 1;
    }

    if (audio_set_output_device(outputDevice) != noErr) {
        printf("Failed to set output device\n");
        vban_cleanup(vban);
        return 1;
    }

    printf("\nVBAN bridge initialized successfully:\n");
    printf("- Remote IP: %s\n", argv[1]);
    printf("- Stream name: %s\n", argv[2]);
    printf("\nPress Ctrl+C to stop...\n");

    // Main loop
    while (running && vban_is_running(vban)) {
        sleep(1);
    }

    // Cleanup
    vban_cleanup(vban);
    printf("VBAN bridge stopped.\n");
    return 0;
} 