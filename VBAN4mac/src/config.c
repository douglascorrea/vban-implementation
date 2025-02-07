#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "vban4mac/config.h"
#include "vban4mac/types.h"
#include "audio.h"

static void trim(char* str) {
    char* start = str;
    char* end = str + strlen(str) - 1;

    while(isspace((unsigned char)*start)) start++;
    while(end > start && isspace((unsigned char)*end)) end--;

    size_t len = (end - start) + 1;
    memmove(str, start, len);
    str[len] = '\0';
}

int load_config(const char* filename, vban_config_t* config) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        perror("Failed to open config file");
        return -1;
    }

    // Set defaults
    strncpy(config->remote_ip, "127.0.0.1", sizeof(config->remote_ip) - 1);
    strncpy(config->stream_name, "Stream1", sizeof(config->stream_name) - 1);
    config->port = VBAN_DEFAULT_PORT;
    config->input_device[0] = '\0';
    config->output_device[0] = '\0';

    char line[256];
    char section[64] = "";

    while (fgets(line, sizeof(line), file)) {
        // Remove newline
        line[strcspn(line, "\n")] = 0;
        trim(line);

        // Skip empty lines and comments
        if (line[0] == '\0' || line[0] == ';' || line[0] == '#')
            continue;

        // Check for section
        if (line[0] == '[') {
            char* end = strchr(line, ']');
            if (end) {
                *end = '\0';
                strncpy(section, line + 1, sizeof(section) - 1);
                continue;
            }
        }

        // Parse key=value
        char* delimiter = strchr(line, '=');
        if (!delimiter)
            continue;

        *delimiter = '\0';
        char* key = line;
        char* value = delimiter + 1;
        trim(key);
        trim(value);

        if (strcmp(section, "network") == 0) {
            if (strcmp(key, "remote_ip") == 0)
                strncpy(config->remote_ip, value, sizeof(config->remote_ip) - 1);
            else if (strcmp(key, "stream_name") == 0)
                strncpy(config->stream_name, value, sizeof(config->stream_name) - 1);
            else if (strcmp(key, "port") == 0)
                config->port = (uint16_t)atoi(value);
        }
        else if (strcmp(section, "audio") == 0) {
            if (strcmp(key, "input_device") == 0)
                strncpy(config->input_device, value, sizeof(config->input_device) - 1);
            else if (strcmp(key, "output_device") == 0)
                strncpy(config->output_device, value, sizeof(config->output_device) - 1);
        }
    }

    fclose(file);
    return 0;
}

AudioDeviceID find_device_by_name(const char* device_name, int is_input) {
    AudioObjectPropertyAddress property = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 dataSize = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &property, 0, NULL, &dataSize);
    if (status != noErr) return 0;

    int deviceCount = dataSize / sizeof(AudioDeviceID);
    AudioDeviceID* devices = malloc(dataSize);
    
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, 0, NULL, &dataSize, devices);
    if (status != noErr) {
        free(devices);
        return 0;
    }

    AudioDeviceID result = 0;
    for (int i = 0; i < deviceCount; i++) {
        char* name = get_device_name(devices[i]);
        if (name) {
            if (strcmp(name, device_name) == 0) {
                // Verify if device has input/output channels as requested
                property.mScope = is_input ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput;
                property.mSelector = kAudioDevicePropertyStreamConfiguration;
                
                status = AudioObjectGetPropertyDataSize(devices[i], &property, 0, NULL, &dataSize);
                if (status == noErr) {
                    AudioBufferList* bufferList = malloc(dataSize);
                    status = AudioObjectGetPropertyData(devices[i], &property, 0, NULL, &dataSize, bufferList);
                    
                    if (status == noErr) {
                        UInt32 channels = 0;
                        for (UInt32 j = 0; j < bufferList->mNumberBuffers; j++) {
                            channels += bufferList->mBuffers[j].mNumberChannels;
                        }
                        if (channels > 0) {
                            result = devices[i];
                        }
                    }
                    free(bufferList);
                }
            }
            free(name);
        }
        if (result) break;
    }

    free(devices);
    return result;
} 