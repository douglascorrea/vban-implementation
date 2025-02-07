#ifndef VBAN4MAC_CONFIG_H
#define VBAN4MAC_CONFIG_H

#include <CoreAudio/CoreAudio.h>

typedef struct {
    char remote_ip[64];
    char stream_name[64];
    uint16_t port;
    char input_device[128];
    char output_device[128];
} vban_config_t;

/**
 * Load configuration from a file
 * @param filename Path to the config file
 * @param config Pointer to config structure to fill
 * @return 0 on success, -1 on error
 */
int load_config(const char* filename, vban_config_t* config);

/**
 * Find audio device ID by name
 * @param device_name Name of the device to find
 * @param is_input 1 for input device, 0 for output device
 * @return Device ID if found, 0 if not found
 */
AudioDeviceID find_device_by_name(const char* device_name, int is_input);

#endif // VBAN4MAC_CONFIG_H 