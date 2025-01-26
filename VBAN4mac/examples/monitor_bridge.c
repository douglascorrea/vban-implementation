#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <math.h>
#include "../include/vban4mac/vban.h"
#include "../src/audio.h"

#define METER_WIDTH 50
#define UPDATE_INTERVAL_MS 50
#define PEAK_HOLD_TIME 20  // Hold peaks for 20 updates

static volatile int running = 1;
static float input_peak = 0.0f;
static float output_peak = 0.0f;
static int input_peak_hold = 0;
static int output_peak_hold = 0;

// ANSI escape codes for colors and cursor control
#define ANSI_CLEAR "\033[2J\033[H"
#define ANSI_RED "\033[31m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_RESET "\033[0m"
#define ANSI_SAVE "\033[s"
#define ANSI_RESTORE "\033[u"

void signal_handler(int signum) {
    printf("\nSignal %d received, stopping...\n", signum);
    running = 0;
}

// Convert linear amplitude to dB
float linear_to_db(float amplitude) {
    return 20.0f * log10f(amplitude + 1e-6f);  // Avoid log(0)
}

// Draw a level meter with peak hold
void draw_meter(const char* label, float level_db, float peak_db, int width) {
    int level_pos = (int)((level_db + 60) * width / 60);  // -60dB to 0dB range
    int peak_pos = (int)((peak_db + 60) * width / 60);
    
    level_pos = level_pos < 0 ? 0 : (level_pos > width ? width : level_pos);
    peak_pos = peak_pos < 0 ? 0 : (peak_pos > width ? width : peak_pos);
    
    printf("%s [", label);
    for (int i = 0; i < width; i++) {
        if (i == peak_pos) {
            printf(ANSI_RED"|"ANSI_RESET);
        }
        else if (i < level_pos) {
            if (i > width * 0.8) printf(ANSI_RED"#"ANSI_RESET);
            else if (i > width * 0.6) printf(ANSI_YELLOW"#"ANSI_RESET);
            else printf(ANSI_GREEN"#"ANSI_RESET);
        }
        else printf(" ");
    }
    printf("] %.1f dB\n", level_db);
}

// Update the display
void update_display(const char* input_device, const char* output_device) {
    printf(ANSI_CLEAR);
    printf("VBAN Audio Monitor\n");
    printf("=================\n\n");
    
    printf("Input Device:  %s\n", input_device);
    printf("Output Device: %s\n\n", output_device);
    
    draw_meter("Input ", linear_to_db(input_peak), linear_to_db(input_peak), METER_WIDTH);
    draw_meter("Output", linear_to_db(output_peak), linear_to_db(output_peak), METER_WIDTH);
    
    printf("\nPress Ctrl+C to exit\n");
    fflush(stdout);
}

// Callback to monitor input levels
void monitor_input_callback(const float* samples, size_t count) {
    float current_peak = 0.0f;
    for (size_t i = 0; i < count; i++) {
        float abs_sample = fabsf(samples[i]);
        if (abs_sample > current_peak) current_peak = abs_sample;
    }
    
    if (current_peak > input_peak) {
        input_peak = current_peak;
        input_peak_hold = PEAK_HOLD_TIME;
    } else if (input_peak_hold > 0) {
        input_peak_hold--;
    } else {
        input_peak *= 0.95f;  // Decay
    }
}

// Callback to monitor output levels
void monitor_output_callback(const float* samples, size_t count) {
    float current_peak = 0.0f;
    for (size_t i = 0; i < count; i++) {
        float abs_sample = fabsf(samples[i]);
        if (abs_sample > current_peak) current_peak = abs_sample;
    }
    
    if (current_peak > output_peak) {
        output_peak = current_peak;
        output_peak_hold = PEAK_HOLD_TIME;
    } else if (output_peak_hold > 0) {
        output_peak_hold--;
    } else {
        output_peak *= 0.95f;  // Decay
    }
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

    // Register our monitoring callbacks
    audio_set_input_monitor(monitor_input_callback);
    audio_set_output_monitor(monitor_output_callback);

    // Get device names for display
    char* input_device_name = NULL;
    char* output_device_name = NULL;
    
    if (input[0] != '\n') {
        if (audio_set_input_device(inputDeviceID) != noErr) {
            printf("Failed to set input device\n");
            vban_cleanup(ctx);
            return 1;
        }
        input_device_name = get_device_name(inputDeviceID);
    } else {
        input_device_name = strdup("Default Input Device");
    }

    if (input[0] != '\n') {
        if (audio_set_output_device(outputDeviceID) != noErr) {
            printf("Failed to set output device\n");
            vban_cleanup(ctx);
            return 1;
        }
        output_device_name = get_device_name(outputDeviceID);
    } else {
        output_device_name = strdup("Default Output Device");
    }

    // Main monitoring loop
    while (running && vban_is_running(ctx)) {
        update_display(input_device_name, output_device_name);
        usleep(UPDATE_INTERVAL_MS * 1000);
    }

    // Cleanup
    free(input_device_name);
    free(output_device_name);
    vban_cleanup(ctx);
    printf(ANSI_CLEAR);  // Clear screen on exit
    return 0;
} 