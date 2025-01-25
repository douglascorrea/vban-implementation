#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <vban4mac/vban.h>

static volatile int running = 1;

static void handle_signal(int sig) {
    printf("\nSignal %d received, stopping...\n", sig);
    running = 0;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("Usage: %s <remote_ip> <stream_name>\n", argv[0]);
        return 1;
    }

    // Set up signal handling
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Initialize VBAN
    vban_handle_t vban = vban_init(argv[1], argv[2]);
    if (!vban) {
        printf("Failed to initialize VBAN\n");
        return 1;
    }

    printf("VBAN bridge initialized successfully:\n");
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