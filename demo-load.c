/*
 * demo-load - Visual test harness for gaudi-monitor
 *
 * Spawns one thread per CPU core with a sinusoidal load pattern offset
 * in phase so all cores show different utilization levels.
 *
 * Build: gcc -O2 -o demo-load demo-load.c -lpthread -lm
 * Usage: ./demo-load                    # CPU load only
 *        ./demo-load --duration 30s     # run for 30 seconds
 *        ./demo-load --until 14:30      # run until 14:30
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>

static volatile int running    = 1;
#define DEFAULT_MAX_SECONDS 300 /* 5 minute failsafe */

static void on_signal(int sig) { (void)sig; running = 0; }

/* Parse "HH:MM" or "HH:MM:SS" into epoch seconds. Returns -1 on error. */
static time_t parse_time(const char *s) {
    int h, m, sec = 0;
    if (sscanf(s, "%d:%d:%d", &h, &m, &sec) < 2) return -1;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = h;
    tm.tm_min  = m;
    tm.tm_sec  = sec;
    time_t target = mktime(&tm);
    if (target <= now) target += 86400;
    return target;
}

/* Parse duration like "30s", "5m", "1h", or bare seconds. Returns seconds, -1 on error. */
static int parse_duration(const char *s) {
    int val = atoi(s);
    if (val <= 0) return -1;
    int len = strlen(s);
    if (len > 0) {
        switch (s[len - 1]) {
        case 'h': case 'H': return val * 3600;
        case 'm': case 'M': return val * 60;
        case 's': case 'S': return val;
        }
    }
    return val;
}

/* ── CPU load ──────────────────────────────────────────────────────── */

typedef struct {
    int core_id;
    int total_cores;
} CpuThreadArg;

static void *cpu_load_thread(void *arg) {
    CpuThreadArg *a = (CpuThreadArg *)arg;
    double phase = (2.0 * M_PI * a->core_id) / a->total_cores;
    double speed = 0.6 + 0.4 * ((double)a->core_id / a->total_cores);

    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (running) {
        double now;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (ts.tv_sec - ts_start.tv_sec) + (ts.tv_nsec - ts_start.tv_nsec) / 1e9;

        /* Sinusoidal target: 5% to 95% utilization */
        double target = 0.50 + 0.45 * sin(now * speed + phase);

        /* Busy/sleep cycle: 50ms window */
        int busy_us = (int)(target * 50000);
        int idle_us = 50000 - busy_us;

        /* Busy spin */
        struct timespec spin_end;
        clock_gettime(CLOCK_MONOTONIC, &spin_end);
        long end_ns = spin_end.tv_nsec + busy_us * 1000L;
        spin_end.tv_sec  += end_ns / 1000000000L;
        spin_end.tv_nsec  = end_ns % 1000000000L;

        while (running) {
            clock_gettime(CLOCK_MONOTONIC, &ts);
            if (ts.tv_sec > spin_end.tv_sec ||
                (ts.tv_sec == spin_end.tv_sec && ts.tv_nsec >= spin_end.tv_nsec))
                break;
            volatile double x = 1.0001;
            for (int j = 0; j < 100; j++) x *= 1.0001;
            (void)x;
        }

        if (idle_us > 0) {
            struct timespec sl = { .tv_sec = 0, .tv_nsec = idle_us * 1000L };
            nanosleep(&sl, NULL);
        }
    }

    free(a);
    return NULL;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    int duration_sec  = 0;
    time_t stop_at    = 0;
    int allow_long_run = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration_sec = parse_duration(argv[++i]);
            if (duration_sec <= 0) {
                fprintf(stderr, "Invalid duration: %s (use e.g. 30s, 5m, 1h)\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--until") == 0 && i + 1 < argc) {
            stop_at = parse_time(argv[++i]);
            if (stop_at < 0) {
                fprintf(stderr, "Invalid time: %s (use HH:MM or HH:MM:SS)\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--allow-long-run") == 0) {
            allow_long_run = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [OPTIONS]\n\n"
                   "Synthetic CPU load generator for testing gaudi-monitor.\n\n"
                   "  --duration TIME    Run for specified time (e.g. 30s, 5m, 1h)\n"
                   "  --until HH:MM      Run until specified time (24h format)\n"
                   "  --allow-long-run   Allow runs longer than 5 minutes\n\n"
                   "Without --duration or --until, stops after 5 minutes unless\n"
                   "--allow-long-run is set.\n\n"
                   "Note: HPU (Gaudi) load generation requires the SynapseAI SDK.\n"
                   "      Install habanalabs-qual and use the built-in benchmarks.\n\n"
                   "https://github.com/your-org/gaudi-monitor\n",
                   argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    /* Determine stop time */
    time_t now = time(NULL);
    if (stop_at > 0) {
        /* --until takes precedence */
    } else if (duration_sec > 0) {
        stop_at = now + duration_sec;
    } else if (!allow_long_run) {
        stop_at = now + DEFAULT_MAX_SECONDS;
    }

    if (stop_at > 0 && !allow_long_run && (stop_at - now) > DEFAULT_MAX_SECONDS) {
        fprintf(stderr, "Error: run time exceeds 5 minutes. Use --allow-long-run to override.\n");
        return 1;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    int ncpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpus < 1) ncpus = 1;

    printf("Starting CPU load on %d cores (sinusoidal, phased)\n", ncpus);

    pthread_t *cpu_threads = calloc(ncpus, sizeof(pthread_t));
    if (!cpu_threads) { perror("calloc"); return 1; }

    for (int i = 0; i < ncpus; i++) {
        CpuThreadArg *a = malloc(sizeof(CpuThreadArg));
        a->core_id     = i;
        a->total_cores = ncpus;
        pthread_create(&cpu_threads[i], NULL, cpu_load_thread, a);
    }

    if (stop_at > 0) {
        int remaining = (int)(stop_at - time(NULL));
        printf("Will stop in %dm %ds (Ctrl-C to stop early)\n",
               remaining / 60, remaining % 60);
    } else {
        printf("Press Ctrl-C to stop\n");
    }

    while (running) {
        if (stop_at > 0 && time(NULL) >= stop_at) {
            printf("\nTime limit reached.\n");
            running = 0;
            break;
        }
        sleep(1);
    }

    printf("\nStopping...\n");
    for (int i = 0; i < ncpus; i++)
        pthread_join(cpu_threads[i], NULL);
    free(cpu_threads);

    printf("Done.\n");
    return 0;
}
