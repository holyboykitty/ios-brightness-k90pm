// SPDX-License-Identifier: GPL-2.0
/*
 * lux_feed.c
 * Goodix GLSX Light Sensor -> ios_brightness bridge
 * Reads Android sensor data via dumpsys sensorservice
 * Feeds lux values to /proc/ios_brightness/lux_feed
 *
 * Compile in Termux:
 *   pkg install clang
 *   clang -O2 -o lux_feed lux_feed.c
 *
 * Usage:
 *   ./lux_feed -v -i 2
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>

#define PROC_LUX  "/proc/ios_brightness/lux_feed"
#define PROC_MODE "/proc/ios_brightness/mode"
#define PROC_EN   "/proc/ios_brightness/enabled"
#define SMOOTH_N  5
#define THRESHOLD 5

static volatile int running = 1;

static void signal_handler(int sig)
{
    running = 0;
}

static int write_proc(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    write(fd, val, strlen(val));
    close(fd);
    return 0;
}

static int read_proc(const char *path, char *buf, int sz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = read(fd, buf, sz - 1);
    close(fd);
    if (n > 0) { buf[n] = '\0'; return n; }
    return -1;
}

/*
 * Parse dumpsys sensorservice for Goodix GLSX ALS data.
 *
 * In the sensor list:
 *   0x00000033) glsx Ambient Light Sensor Non-wakeup | goodix
 *
 * In the event log:
 *   last 10 events:
 *     0) 234.56, 0.00, 0.00, ...
 */
static int read_lux(void)
{
    FILE *fp;
    char line[1024];
    int found = 0;
    int lux = -1;

    fp = popen("dumpsys sensorservice 2>/dev/null", "r");
    if (!fp) return -1;

    while (fgets(line, sizeof(line), fp)) {
        /* Find the Goodix GLSX ALS sensor block */
        if (strstr(line, "glsx") &&
            strstr(line, "Ambient Light") &&
            strstr(line, "Non-wakeup")) {
            found = 1;
            continue;
        }

        /* In that block, find "last N events:" then read first event */
        if (found && strstr(line, "last") && strstr(line, "events")) {
            if (fgets(line, sizeof(line), fp)) {
                char *p = strchr(line, ')');
                if (p) {
                    float f;
                    if (sscanf(p + 1, " %f", &f) == 1 && f >= 0)
                        lux = (int)(f + 0.5f);
                }
            }
            break;
        }

        /* If we hit another sensor, reset */
        if (found && strstr(line, "0x") && strstr(line, ")") &&
            !strstr(line, "glsx"))
            found = 0;
    }

    pclose(fp);
    return lux;
}

static int smooth(int *buf, int *idx, int val)
{
    int i, sum;
    buf[*idx % SMOOTH_N] = val;
    (*idx)++;
    for (i = 0, sum = 0; i < SMOOTH_N; i++)
        sum += buf[i];
    return sum / SMOOTH_N;
}

static int is_active(void)
{
    char e[8] = {0}, m[16] = {0};
    if (read_proc(PROC_EN, e, sizeof(e)) < 0) return 0;
    if (read_proc(PROC_MODE, m, sizeof(m)) < 0) return 0;
    return (e[0] == '1') && !strncmp(m, "auto", 4);
}

int main(int argc, char *argv[])
{
    int smooth_buf[SMOOTH_N] = {0};
    int idx = 0, prev = -1;
    int poll_sec = 2;
    int verbose = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
            verbose = 1;
        else if (!strcmp(argv[i], "-i") && i + 1 < argc)
            poll_sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr,
                "lux_feed - Goodix GLSX -> ios_brightness bridge\n\n"
                "Usage: %s [-v] [-i poll_sec]\n\n"
                "Options:\n"
                "  -v          verbose output\n"
                "  -i <sec>    poll interval in seconds (default: 2)\n"
                "  -h          show this help\n\n"
                "This daemon reads the Goodix GLSX ambient light sensor\n"
                "via dumpsys sensorservice and feeds lux values to\n"
                "/proc/ios_brightness/lux_feed for auto-brightness.\n",
                argv[0]);
            return 0;
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    fprintf(stderr, "lux_feed: started (poll=%ds)\n", poll_sec);

    /* Wait for kernel module to be ready */
    while (running) {
        struct stat st;
        if (stat(PROC_LUX, &st) == 0) break;
        if (verbose)
            fprintf(stderr, "lux_feed: waiting for module...\n");
        sleep(2);
    }

    if (!running) {
        fprintf(stderr, "lux_feed: interrupted\n");
        return 1;
    }

    fprintf(stderr, "lux_feed: module ready, entering main loop\n");

    while (running) {
        /* Only read sensor when module is enabled and in auto mode */
        if (!is_active()) {
            sleep(poll_sec);
            continue;
        }

        int lux = read_lux();

        if (lux >= 0) {
            int sm = smooth(smooth_buf, &idx, lux);

            /* Only write when value changes significantly */
            if (abs(sm - prev) >= THRESHOLD || prev < 0) {
                char val[16];
                snprintf(val, sizeof(val), "%d", sm);
                write_proc(PROC_LUX, val);
                prev = sm;
                if (verbose)
                    fprintf(stderr, "lux: raw=%d smooth=%d\n",
                            lux, sm);
            }
        } else if (verbose) {
            fprintf(stderr, "lux: read failed\n");
        }

        sleep(poll_sec);
    }

    fprintf(stderr, "lux_feed: stopped\n");
    return 0;
}
