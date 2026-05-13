#include "capture.h"
#include "ascii.h"
#include "timing.h"
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// Defaults
#define DEFAULT_ASCII_WIDTH  80
#define DEFAULT_ASCII_HEIGHT 40
#define DEFAULT_CAPTURE_WIDTH  160
#define DEFAULT_CAPTURE_HEIGHT 120
#define DEFAULT_FPS 20

volatile sig_atomic_t keep_running = 1;
void handle_signal(int sig) { (void)sig; keep_running = 0; }

void print_usage(char *prog) {
    fprintf(stderr,
        "Usage: %s [-d <device>] [-W <width>] [-H <height>] [-f <fps>]\n",
        prog);
    fprintf(stderr, "  -d <device>  : video device (default: /dev/video0)\n");
    fprintf(stderr, "  -W <width>   : ASCII output width (default: %d)\n", DEFAULT_ASCII_WIDTH);
    fprintf(stderr, "  -H <height>  : ASCII output height (default: %d)\n", DEFAULT_ASCII_HEIGHT);
    fprintf(stderr, "  -f <fps>     : target framerate (default: %d)\n", DEFAULT_FPS);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, handle_signal);

    // Config
    char *device = "/dev/video0";
    int ascii_w = DEFAULT_ASCII_WIDTH;
    int ascii_h = DEFAULT_ASCII_HEIGHT;
    int fps = DEFAULT_FPS;

    int opt;
    while ((opt = getopt(argc, argv, "d:W:H:f:")) != -1) {
        switch (opt) {
            case 'd': device = optarg; break;
            case 'W': ascii_w = atoi(optarg); if (ascii_w <= 0) ascii_w = DEFAULT_ASCII_WIDTH; break;
            case 'H': ascii_h = atoi(optarg); if (ascii_h <= 0) ascii_h = DEFAULT_ASCII_HEIGHT; break;
            case 'f': fps = atoi(optarg); if (fps <= 0) fps = DEFAULT_FPS; break;
            default: print_usage(argv[0]); return 1;
        }
    }

    // Capture resolution (fixed for simplicity, could also be made configurable)
    int cap_w = DEFAULT_CAPTURE_WIDTH;
    int cap_h = DEFAULT_CAPTURE_HEIGHT;

    timing_init(fps);

    webcam_t cam = { .fd = -1, .buffer = MAP_FAILED };
    if (webcam_init(&cam, device, cap_w, cap_h) < 0) {
        perror("webcam_init");
        return 1;
    }
    printf("Webcam opened: %s, capture resolution %dx%d\n", device, cam.width, cam.height);

    uint8_t *gray = malloc(cam.width * cam.height);
    if (!gray) {
        perror("malloc gray");
        webcam_cleanup(&cam);
        return 1;
    }

    printf("Starting ASCII stream (%dx%d), target %d fps. Press Ctrl+C to stop.\n",
           ascii_w, ascii_h, fps);

    struct timespec start_time;
    while (keep_running) {
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        if (webcam_wait_frame(&cam, 1000) < 0) {
            // timeout or error, just continue
            continue;
        }

        if (webcam_capture_frame(&cam, gray) < 0) {
            perror("capture_frame");
            break;
        }

        char *ascii_art = grayscale_to_ascii(gray, cam.width, cam.height,
                                             ascii_w, ascii_h);
        if (ascii_art) {
            printf("\033[2J\033[H");   // clear screen, home cursor
            fputs(ascii_art, stdout);
            fflush(stdout);
            free(ascii_art);
        }

        if (webcam_requeue_buffer(&cam) < 0) {
            perror("requeue_buffer");
            break;
        }

        timing_sleep(&start_time);
    }

    printf("\nStopping...\n");
    free(gray);
    webcam_cleanup(&cam);
    return 0;
}
