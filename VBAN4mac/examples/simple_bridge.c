#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <CoreAudio/CoreAudio.h>
#include <vban4mac/vban.h>
#include <vban4mac/types.h>
#include <vban4mac/config.h>
#include "../src/audio.h"
#include <string.h>
#include <syslog.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig) {
    (void)sig;  // Unused parameter
    running = 0;
}

static void daemonize(const char* pid_file) {
    // First fork (detaches from parent)
    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "Failed to fork first time");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    // Create new session
    if (setsid() < 0) {
        syslog(LOG_ERR, "Failed to create new session");
        exit(EXIT_FAILURE);
    }

    // Second fork (relinquish session leadership)
    pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "Failed to fork second time");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    // Set new file permissions
    umask(0);

    // Change to root directory
    chdir("/");

    // Close all open file descriptors
    for (int x = sysconf(_SC_OPEN_MAX); x >= 0; x--) {
        close(x);
    }

    // Redirect standard files to /dev/null
    open("/dev/null", O_RDWR);
    dup(0);
    dup(0);

    // Write PID file
    FILE* pid_fp = fopen(pid_file, "w");
    if (pid_fp) {
        fprintf(pid_fp, "%d\n", getpid());
        fclose(pid_fp);
    }
}

// Add this function to slugify the stream name
static void slugify(const char* input, char* output, size_t output_size) {
    size_t i = 0, j = 0;
    
    // Convert to lowercase and replace non-alphanumeric chars with hyphen
    while (input[i] && j < output_size - 1) {
        char c = tolower((unsigned char)input[i]);
        if (isalnum(c)) {
            output[j++] = c;
        } else if (j > 0 && output[j-1] != '-') {
            output[j++] = '-';
        }
        i++;
    }
    
    // Remove trailing hyphen if exists
    if (j > 0 && output[j-1] == '-') {
        j--;
    }
    
    output[j] = '\0';
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        printf("Usage: %s <config_file>\n", argv[0]);
        return 1;
    }

    // Initialize syslog
    openlog("vban_bridge", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    // Load configuration
    vban_config_t config;
    if (load_config(argv[1], &config) != 0) {
        syslog(LOG_ERR, "Failed to load configuration");
        return 1;
    }

    // Create PID filename based on slugified stream name
    char slug[64];
    slugify(config.stream_name, slug, sizeof(slug));
    
    char pid_file[128];
    snprintf(pid_file, sizeof(pid_file), "/tmp/vban_bridge_%s.pid", slug);

    // Daemonize the process
    daemonize(pid_file);

    // Set up signal handling
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);
    signal(SIGINT, handle_signal);

    // Get devices from config
    AudioDeviceID inputDevice = 0, outputDevice = 0;
    
    if (strlen(config.input_device) > 0) {
        inputDevice = find_device_by_name(config.input_device, 1);
        if (!inputDevice) {
            syslog(LOG_ERR, "Configured input device '%s' not found", config.input_device);
            goto cleanup;
        }
        syslog(LOG_INFO, "Using input device: %s", config.input_device);
    } else {
        syslog(LOG_ERR, "No input device configured");
        goto cleanup;
    }

    if (strlen(config.output_device) > 0) {
        outputDevice = find_device_by_name(config.output_device, 0);
        if (!outputDevice) {
            syslog(LOG_ERR, "Configured output device '%s' not found", config.output_device);
            goto cleanup;
        }
        syslog(LOG_INFO, "Using output device: %s", config.output_device);
    } else {
        syslog(LOG_ERR, "No output device configured");
        goto cleanup;
    }

    // Initialize VBAN
    vban_handle_t vban = vban_init_with_port(config.remote_ip, config.stream_name, config.port);
    if (!vban) {
        syslog(LOG_ERR, "Failed to initialize VBAN");
        goto cleanup;
    }

    // Set input and output devices
    if (audio_set_input_device(inputDevice) != noErr) {
        syslog(LOG_ERR, "Failed to set input device");
        vban_cleanup(vban);
        goto cleanup;
    }

    if (audio_set_output_device(outputDevice) != noErr) {
        syslog(LOG_ERR, "Failed to set output device");
        vban_cleanup(vban);
        goto cleanup;
    }

    syslog(LOG_INFO, "VBAN bridge started - IP: %s, Stream: %s, Port: %u",
           config.remote_ip, config.stream_name, config.port);

    // Main loop
    while (running && vban_is_running(vban)) {
        sleep(1);
    }

    // Cleanup
    syslog(LOG_INFO, "VBAN bridge stopping...");
    vban_cleanup(vban);
    syslog(LOG_INFO, "VBAN bridge stopped");

cleanup:
    // Remove PID file
    unlink(pid_file);
    closelog();
    return 0;
} 