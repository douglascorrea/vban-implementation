#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <CoreAudio/CoreAudio.h>
#include <vban4mac/vban.h>
#include <vban4mac/types.h>
#include "../src/audio.h"
#include <string.h>
#include <vban4mac/config.h>

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig) {
    (void)sig;  // Unused parameter
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
    if (argc != 2) {
        printf("Usage: %s <config_file>\n", argv[0]);
        return 1;
    }

    // Load configuration
    vban_config_t config;
    if (load_config(argv[1], &config) != 0) {
        printf("Failed to load configuration\n");
        return 1;
    }

    // List available audio devices
    printf("\nAvailable Audio Devices:\n");
    audio_list_devices();

    // Get devices from config or prompt user
    AudioDeviceID inputDevice, outputDevice;
    
    if (strlen(config.input_device) > 0) {
        inputDevice = find_device_by_name(config.input_device, 1);
        if (!inputDevice) {
            printf("Warning: Configured input device '%s' not found\n", config.input_device);
            inputDevice = get_user_device_selection("input");
        } else {
            printf("Using configured input device: %s\n", config.input_device);
        }
    } else {
        inputDevice = get_user_device_selection("input");
    }

    if (strlen(config.output_device) > 0) {
        outputDevice = find_device_by_name(config.output_device, 0);
        if (!outputDevice) {
            printf("Warning: Configured output device '%s' not found\n", config.output_device);
            outputDevice = get_user_device_selection("output");
        } else {
            printf("Using configured output device: %s\n", config.output_device);
        }
    } else {
        outputDevice = get_user_device_selection("output");
    }

    // Set up signal handling
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Initialize VBAN
    vban_handle_t vban = vban_init_with_port(config.remote_ip, config.stream_name, config.port);
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
    printf("- Remote IP: %s\n", config.remote_ip);
    printf("- Stream name: %s\n", config.stream_name);
    printf("- Port: %u\n", config.port);
    printf("\nPress Ctrl+C to stop...\n");

    // Main loop
    while (running && vban_is_running(vban)) {
        sleep(1);
    }

    // Signal received or VBAN stopped
    printf("\nSignal received, initiating shutdown...\n");
    printf("Stopping VBAN bridge...\n");
    vban_cleanup(vban);
    printf("VBAN bridge stopped.\n");
    return 0;
} 