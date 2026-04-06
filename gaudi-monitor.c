/*
 * gaudi-monitor - System monitor for Intel Gaudi AI Accelerators
 *
 * Displays CPU per-core usage, memory, CPU thermals, HPU utilization,
 * HPU temperature/power/clock, and HPU processes in a single TUI.
 *
 * Build: gcc -O2 -o gaudi-monitor gaudi-monitor.c -lncursesw -ldl -lpthread
 */

#ifndef VERSION
#define VERSION "dev"
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <pwd.h>
#include <ncurses.h>
#include <locale.h>
#include <sys/sysinfo.h>
#include <getopt.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>

/* ── HLML types (loaded dynamically) ────────────────────────────────── */

typedef void *hlmlDevice_t;
typedef int   hlmlReturn_t;

typedef struct {
    unsigned int memory; /* field 0 (misnamed in HLML on this version) */
    unsigned int aip;    /* field 1 = actual AI Processor utilization % */
} hlmlUtilization_t;

typedef struct {
    unsigned long long free;
    unsigned long long total;
    unsigned long long used;
} hlmlMemory_t;

typedef struct {
    unsigned int pid;
    unsigned long long usedHlMemory;
} hlmlProcessInfo_t;

#define HLML_SUCCESS              0
#define HLML_TEMPERATURE_AIP      0   /* AI Processor die temperature */
#define HLML_CLOCK_AIC            0   /* AI Core clock */
#define HLML_CLOCK_MME            1   /* Matrix Multiplication Engine clock */
#define HLML_CLOCK_SOC            2   /* SoC clock */

/* HLML function pointers */
static hlmlReturn_t (*pHlmlInit)(void);
static hlmlReturn_t (*pHlmlShutdown)(void);
static hlmlReturn_t (*pHlmlDeviceGetCount)(unsigned int *);
static hlmlReturn_t (*pHlmlDeviceGetHandleByIndex)(unsigned int, hlmlDevice_t *);
static hlmlReturn_t (*pHlmlDeviceGetName)(hlmlDevice_t, char *, unsigned int);
static hlmlReturn_t (*pHlmlDeviceGetUtilizationRates)(hlmlDevice_t, hlmlUtilization_t *);
static hlmlReturn_t (*pHlmlDeviceGetMemoryInfo)(hlmlDevice_t, hlmlMemory_t *);
static hlmlReturn_t (*pHlmlDeviceGetTemperature)(hlmlDevice_t, int, unsigned int *);
static hlmlReturn_t (*pHlmlDeviceGetPowerUsage)(hlmlDevice_t, unsigned int *);
static hlmlReturn_t (*pHlmlDeviceGetClockInfo)(hlmlDevice_t, int, unsigned int *);
static hlmlReturn_t (*pHlmlDeviceGetComputeRunningProcesses)(hlmlDevice_t, unsigned int *, hlmlProcessInfo_t *);

static void *hlml_handle  = NULL;
static int   hlml_ok      = 0;
static unsigned int hpu_count = 0;   /* number of Gaudi HPUs detected */
static char  cpu_model_name[128] = "";

/* ── Constants ──────────────────────────────────────────────────────── */

#define MAX_CPUS      512
#define MAX_HPU_PROCS 64
#define REFRESH_MS    1000
#define COLOR_GRAY    8
#define HISTORY_LEN   20

/* ── CPU state ──────────────────────────────────────────────────────── */

typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
} CpuTick;

static int      num_cpus = 0;
static CpuTick  prev_ticks[MAX_CPUS + 1]; /* index 0 = aggregate */
static double   cpu_pct[MAX_CPUS + 1];
static unsigned int cpu_part[MAX_CPUS];   /* ARM CPU part IDs */

/* ── HPU process info ───────────────────────────────────────────────── */

typedef struct {
    unsigned int  pid;
    unsigned long long mem_bytes;
    char          name[256];
    char          user[64];
    double        cpu_pct;
} HpuProc;

/* ── Per-process CPU tracking ───────────────────────────────────────── */

#define MAX_TRACKED_PIDS 128

typedef struct {
    unsigned int  pid;
    unsigned long long ticks;
} ProcCpuSnap;

static ProcCpuSnap prev_proc_snaps[MAX_TRACKED_PIDS];
static int         prev_proc_count = 0;
static unsigned long long prev_total_cpu_ticks = 0;

static unsigned long long read_proc_cpu_ticks(unsigned int pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[1024];
    int n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    char *cp = strrchr(buf, ')');
    if (!cp) return 0;
    cp += 2;
    unsigned long utime = 0, stime = 0;
    sscanf(cp, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
           &utime, &stime);
    return utime + stime;
}

static unsigned long long read_total_cpu_ticks_sum(void) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0;
    char line[512];
    unsigned long long sum = 0;
    if (fgets(line, sizeof(line), f)) {
        unsigned long long u, n, s, id, io, ir, si, st;
        sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
               &u, &n, &s, &id, &io, &ir, &si, &st);
        sum = u + n + s + id + io + ir + si + st;
    }
    fclose(f);
    return sum;
}

static double calc_proc_cpu_pct(unsigned int pid) {
    unsigned long long cur_ticks = read_proc_cpu_ticks(pid);
    unsigned long long cur_total = read_total_cpu_ticks_sum();
    unsigned long long total_delta = cur_total - prev_total_cpu_ticks;

    for (int i = 0; i < prev_proc_count; i++) {
        if (prev_proc_snaps[i].pid == pid) {
            unsigned long long proc_delta = cur_ticks - prev_proc_snaps[i].ticks;
            if (total_delta > 0)
                return (double)proc_delta / (double)total_delta * 100.0 * num_cpus;
            return 0.0;
        }
    }
    return 0.0;
}

static void update_proc_cpu_snapshots(HpuProc *procs, int count) {
    prev_proc_count = 0;
    for (int i = 0; i < count && prev_proc_count < MAX_TRACKED_PIDS; i++) {
        prev_proc_snaps[prev_proc_count].pid   = procs[i].pid;
        prev_proc_snaps[prev_proc_count].ticks = read_proc_cpu_ticks(procs[i].pid);
        prev_proc_count++;
    }
    prev_total_cpu_ticks = read_total_cpu_ticks_sum();
}

/* ── History ring buffers ────────────────────────────────────────────── */

static double cpu_history[HISTORY_LEN];
static double hpu_history[HISTORY_LEN];
static int    history_pos   = 0;
static int    history_count = 0;

/* ── Globals ────────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_quit = 0;
static int sort_mode = 0; /* 0=by mem, 1=by pid */
static int delay_ms  = REFRESH_MS;
static double last_hpu_util = 0;

/* Command-line options */
static FILE       *log_fp          = NULL;
static int         log_interval_ms = 1000;
static int         no_ui           = 0;
static int         prom_port       = 0;
static const char *prom_token      = NULL;

/* ── Signal handler ─────────────────────────────────────────────────── */

static void on_signal(int sig) { (void)sig; g_quit = 1; }

/* ── HLML loading ───────────────────────────────────────────────────── */

static int load_hlml(void) {
    const char *paths[] = {
        "libhlml.so.1",
        "libhlml.so",
        "/usr/lib/habanalabs/libhlml.so.1",
        "/usr/lib/habanalabs/libhlml.so",
        "/usr/lib/x86_64-linux-gnu/libhlml.so.1",
        "/opt/habanalabs/qual/gaudi2/bin/libhlml.so",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        hlml_handle = dlopen(paths[i], RTLD_LAZY);
        if (hlml_handle) break;
    }
    if (!hlml_handle) return -1;

    #define LOAD(ptr, ...) do { \
        const char *_names[] = { __VA_ARGS__, NULL }; \
        for (int _i = 0; _names[_i]; _i++) { \
            *(void **)(&ptr) = dlsym(hlml_handle, _names[_i]); \
            if (ptr) break; \
        } \
    } while(0)

    LOAD(pHlmlInit,                             "hlmlInit",                             "hlml_init");
    LOAD(pHlmlShutdown,                         "hlmlShutdown",                         "hlml_shutdown");
    LOAD(pHlmlDeviceGetCount,                   "hlmlDeviceGetCount",                   "hlml_device_get_count");
    LOAD(pHlmlDeviceGetHandleByIndex,           "hlmlDeviceGetHandleByIndex",           "hlml_device_get_handle_by_index");
    LOAD(pHlmlDeviceGetName,                    "hlmlDeviceGetName",                    "hlml_device_get_name");
    LOAD(pHlmlDeviceGetUtilizationRates,        "hlmlDeviceGetUtilizationRates",        "hlml_device_get_utilization_rates");
    LOAD(pHlmlDeviceGetMemoryInfo,              "hlmlDeviceGetMemoryInfo",              "hlml_device_get_memory_info");
    LOAD(pHlmlDeviceGetTemperature,             "hlmlDeviceGetTemperature",             "hlml_device_get_temperature");
    LOAD(pHlmlDeviceGetPowerUsage,              "hlmlDeviceGetPowerUsage",              "hlml_device_get_power_usage");
    LOAD(pHlmlDeviceGetClockInfo,               "hlmlDeviceGetClockInfo",               "hlml_device_get_clock_info");
    LOAD(pHlmlDeviceGetComputeRunningProcesses, "hlmlDeviceGetComputeRunningProcesses", "hlml_device_get_compute_running_processes");
    #undef LOAD

    if (!pHlmlInit) return -1;
    if (pHlmlInit() != HLML_SUCCESS) return -1;

    return 0;
}

/* ── Sysfs fallback: read HPU info from /sys/class/habanalabs ────────── */
/* Used when HLML is not available */

#define MAX_SYSFS_HPUS 16
static char sysfs_hpu_names[MAX_SYSFS_HPUS][64];
static int  sysfs_hpu_count = 0;

static void read_sysfs_hpu_names(void) {
    sysfs_hpu_count = 0;
    DIR *d = opendir("/sys/class/habanalabs");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && sysfs_hpu_count < MAX_SYSFS_HPUS) {
        if (strncmp(e->d_name, "hl", 2) != 0) continue;
        /* Read device_type if available */
        char path[128];
        snprintf(path, sizeof(path), "/sys/class/habanalabs/%s/device_type", e->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            char dtype[64] = "";
            if (fgets(dtype, sizeof(dtype), f)) {
                dtype[strcspn(dtype, "\n\r")] = '\0';
                snprintf(sysfs_hpu_names[sysfs_hpu_count], 64, "%s", dtype);
            }
            fclose(f);
        } else {
            snprintf(sysfs_hpu_names[sysfs_hpu_count], 64, "Gaudi HPU");
        }
        sysfs_hpu_count++;
    }
    closedir(d);
}

/* ── CPU core type identification ───────────────────────────────────── */

static void read_cpu_model_name(void) {
    FILE *f = fopen("/sys/firmware/devicetree/base/model", "r");
    if (f) {
        if (fgets(cpu_model_name, sizeof(cpu_model_name), f))
            cpu_model_name[strcspn(cpu_model_name, "\n\r")] = '\0';
        fclose(f);
        if (cpu_model_name[0]) return;
    }

    f = fopen("/sys/devices/virtual/dmi/id/product_name", "r");
    if (f) {
        if (fgets(cpu_model_name, sizeof(cpu_model_name), f))
            cpu_model_name[strcspn(cpu_model_name, "\n\r")] = '\0';
        fclose(f);
        for (char *p = cpu_model_name; *p; p++)
            if (*p == '_') *p = ' ';
        if (cpu_model_name[0]) return;
    }

    f = fopen("/proc/cpuinfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *sep = strchr(line, ':');
        if (!sep) continue;
        if (strncmp(line, "model name", 10) == 0) {
            const char *val = sep + 1;
            while (*val == ' ' || *val == '\t') val++;
            snprintf(cpu_model_name, sizeof(cpu_model_name), "%s", val);
            cpu_model_name[strcspn(cpu_model_name, "\n\r")] = '\0';
            break;
        }
    }
    fclose(f);
}

static void read_cpu_part_ids(void) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return;
    char line[256];
    int cur_cpu = -1;
    while (fgets(line, sizeof(line), f)) {
        int n;
        if (sscanf(line, "processor : %d", &n) == 1) {
            cur_cpu = n;
        } else if (cur_cpu >= 0 && cur_cpu < MAX_CPUS) {
            unsigned int part;
            if (sscanf(line, "CPU part : %x", &part) == 1)
                cpu_part[cur_cpu] = part;
        }
    }
    fclose(f);
}

static const char *cpu_part_label(int cpu_idx) {
    switch (cpu_part[cpu_idx]) {
    case 0xd85: return "X925";
    case 0xd87: return "X725";
    case 0xd44: return "X4";
    case 0xd43: return "A720";
    case 0xd46: return "A725";
    case 0xd41: return "A78";
    case 0xd40: return "V2";
    default:    return "";
    }
}

/* ── CPU sampling ───────────────────────────────────────────────────── */

static void read_cpu_ticks(CpuTick ticks[], int *n_cpus) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;

    char line[512];
    int idx = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0) continue;
        CpuTick t = {0};
        if (line[3] == ' ') {
            sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &t.user, &t.nice, &t.system, &t.idle,
                   &t.iowait, &t.irq, &t.softirq, &t.steal);
            ticks[0] = t;
        } else {
            int cpunum;
            sscanf(line + 3, "%d", &cpunum);
            sscanf(strchr(line + 3, ' ') + 1, "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &t.user, &t.nice, &t.system, &t.idle,
                   &t.iowait, &t.irq, &t.softirq, &t.steal);
            if (cpunum + 1 < MAX_CPUS) {
                ticks[cpunum + 1] = t;
                idx = cpunum + 1;
            }
        }
    }
    *n_cpus = idx;
    fclose(f);
}

static void compute_cpu_usage(void) {
    CpuTick cur[MAX_CPUS + 1];
    int n = 0;
    read_cpu_ticks(cur, &n);
    num_cpus = n;

    for (int i = 0; i <= n; i++) {
        unsigned long long prev_idle  = prev_ticks[i].idle + prev_ticks[i].iowait;
        unsigned long long cur_idle   = cur[i].idle + cur[i].iowait;
        unsigned long long prev_total = prev_ticks[i].user + prev_ticks[i].nice +
                                        prev_ticks[i].system + prev_ticks[i].idle +
                                        prev_ticks[i].iowait + prev_ticks[i].irq +
                                        prev_ticks[i].softirq + prev_ticks[i].steal;
        unsigned long long cur_total  = cur[i].user + cur[i].nice +
                                        cur[i].system + cur[i].idle +
                                        cur[i].iowait + cur[i].irq +
                                        cur[i].softirq + cur[i].steal;
        unsigned long long totald = cur_total - prev_total;
        unsigned long long idled  = cur_idle  - prev_idle;
        if (totald == 0)
            cpu_pct[i] = 0.0;
        else
            cpu_pct[i] = (double)(totald - idled) / (double)totald * 100.0;
    }

    memcpy(prev_ticks, cur, sizeof(cur));
}

/* ── Memory info ────────────────────────────────────────────────────── */

typedef struct {
    unsigned long long total_kb;
    unsigned long long free_kb;
    unsigned long long avail_kb;
    unsigned long long buffers_kb;
    unsigned long long cached_kb;
    unsigned long long swap_total_kb;
    unsigned long long swap_free_kb;
    unsigned long long app_kb;
    unsigned long long bufcache_kb;
    unsigned long long swap_used_kb;
} MemInfo;

static void meminfo_calc(MemInfo *m) {
    m->bufcache_kb = m->buffers_kb + m->cached_kb;
    m->app_kb = (m->total_kb > m->free_kb + m->bufcache_kb)
              ? m->total_kb - m->free_kb - m->bufcache_kb : 0;
    m->swap_used_kb = (m->swap_total_kb > m->swap_free_kb)
                    ? m->swap_total_kb - m->swap_free_kb : 0;
}

static void read_meminfo(MemInfo *m) {
    memset(m, 0, sizeof(*m));
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %llu kB",    &m->total_kb)     == 1) continue;
        if (sscanf(line, "MemFree: %llu kB",     &m->free_kb)      == 1) continue;
        if (sscanf(line, "MemAvailable: %llu kB",&m->avail_kb)     == 1) continue;
        if (sscanf(line, "Buffers: %llu kB",     &m->buffers_kb)   == 1) continue;
        if (sscanf(line, "Cached: %llu kB",      &m->cached_kb)    == 1) continue;
        if (sscanf(line, "SwapTotal: %llu kB",   &m->swap_total_kb)== 1) continue;
        if (sscanf(line, "SwapFree: %llu kB",    &m->swap_free_kb) == 1) continue;
    }
    fclose(f);
    meminfo_calc(m);
}

/* ── CPU thermals ───────────────────────────────────────────────────── */

static int read_cpu_temp(void) {
    int max_temp = 0;
    for (int i = 0; i < 20; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", i);
        FILE *f = fopen(path, "r");
        if (!f) break;
        int t = 0;
        if (fscanf(f, "%d", &t) == 1 && t > max_temp)
            max_temp = t;
        fclose(f);
    }
    return max_temp / 1000;
}

/* ── CPU frequency ──────────────────────────────────────────────────── */

static int read_cpu_freq_mhz(void) {
    /* Try sysfs first */
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (f) {
        int khz = 0;
        (void)!fscanf(f, "%d", &khz);
        fclose(f);
        if (khz > 0) return khz / 1000;
    }
    /* Fallback: parse "cpu MHz" from /proc/cpuinfo */
    f = fopen("/proc/cpuinfo", "r");
    if (!f) return 0;
    char line[256];
    double mhz = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "cpu MHz : %lf", &mhz) == 1 ||
            sscanf(line, "CPU MHz: %lf", &mhz) == 1)
            break;
    }
    fclose(f);
    return (int)mhz;
}

/* ── Process name lookup ────────────────────────────────────────────── */

static void get_proc_name(unsigned int pid, char *buf, int len) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/comm", pid);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(buf, len, f)) {
            char *nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
        }
        fclose(f);
    } else {
        snprintf(buf, len, "[pid %u]", pid);
    }
}

static void get_proc_cmdline(unsigned int pid, char *buf, int len) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/cmdline", pid);
    FILE *f = fopen(path, "r");
    if (f) {
        int n = fread(buf, 1, len - 1, f);
        fclose(f);
        if (n > 0) {
            buf[n] = '\0';
            for (int i = 0; i < n - 1; i++)
                if (buf[i] == '\0') buf[i] = ' ';
            char *space = strchr(buf, ' ');
            char *slash = NULL;
            if (space)
                slash = memrchr(buf, '/', space - buf);
            else
                slash = strrchr(buf, '/');
            if (slash)
                memmove(buf, slash + 1, strlen(slash + 1) + 1);
            return;
        }
    }
    get_proc_name(pid, buf, len);
}

static void get_proc_user(unsigned int pid, char *buf, int len) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(buf, len, "?"); return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned int uid;
        if (sscanf(line, "Uid:\t%u", &uid) == 1) {
            struct passwd *pw = getpwuid(uid);
            if (pw)
                snprintf(buf, len, "%s", pw->pw_name);
            else
                snprintf(buf, len, "%u", uid);
            fclose(f);
            return;
        }
    }
    fclose(f);
    snprintf(buf, len, "?");
}

/* ── Drawing helpers ────────────────────────────────────────────────── */

static void draw_bar(int y, int x, int width, double pct, int color_pair) {
    int filled = (int)(pct / 100.0 * width + 0.5);
    if (filled > width) filled = width;

    move(y, x);
    attron(COLOR_PAIR(8));
    addch('[');
    attroff(COLOR_PAIR(8));

    attron(COLOR_PAIR(color_pair));
    for (int i = 0; i < filled; i++)
        addch(ACS_BLOCK);
    attroff(COLOR_PAIR(color_pair));

    attron(COLOR_PAIR(8));
    for (int i = filled; i < width; i++)
        addch(ACS_BULLET);
    addch(']');
    attroff(COLOR_PAIR(8));
}

static void draw_bar_segmented(int y, int x, int width,
                               double pct_used, double pct_bufcache,
                               int color_used, int color_cache) {
    int filled_used  = (int)(pct_used     / 100.0 * width + 0.5);
    int filled_cache = (int)(pct_bufcache / 100.0 * width + 0.5);
    if (filled_used + filled_cache > width)
        filled_cache = width - filled_used;

    move(y, x);
    attron(COLOR_PAIR(8));
    addch('[');
    attroff(COLOR_PAIR(8));

    attron(COLOR_PAIR(color_used));
    for (int i = 0; i < filled_used; i++) addch(ACS_BLOCK);
    attroff(COLOR_PAIR(color_used));

    attron(COLOR_PAIR(color_cache));
    for (int i = 0; i < filled_cache; i++) addch(ACS_BLOCK);
    attroff(COLOR_PAIR(color_cache));

    attron(COLOR_PAIR(8));
    for (int i = filled_used + filled_cache; i < width; i++) addch(ACS_BULLET);
    addch(']');
    attroff(COLOR_PAIR(8));
}

static const char *fmt_bytes(unsigned long long bytes, char *buf, int len) {
    if (bytes >= (1ULL << 30))
        snprintf(buf, len, "%.1fG", (double)bytes / (1ULL << 30));
    else if (bytes >= (1ULL << 20))
        snprintf(buf, len, "%.1fM", (double)bytes / (1ULL << 20));
    else if (bytes >= (1ULL << 10))
        snprintf(buf, len, "%.1fK", (double)bytes / (1ULL << 10));
    else
        snprintf(buf, len, "%lluB", bytes);
    return buf;
}

/* ── Uptime ─────────────────────────────────────────────────────────── */

static void fmt_uptime(char *buf, int len) {
    struct sysinfo si;
    if (sysinfo(&si) != 0) { snprintf(buf, len, "?"); return; }
    long s = si.uptime;
    int days = s / 86400; s %= 86400;
    int hrs  = s / 3600;  s %= 3600;
    int mins = s / 60;
    if (days > 0)
        snprintf(buf, len, "%dd %dh %dm", days, hrs, mins);
    else
        snprintf(buf, len, "%dh %dm", hrs, mins);
}

/* ── Load average ───────────────────────────────────────────────────── */

static void get_loadavg(double *l1, double *l5, double *l15) {
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) { (void)!fscanf(f, "%lf %lf %lf", l1, l5, l15); fclose(f); }
}

/* ── History chart ──────────────────────────────────────────────────── */

static const char *block_chars[] = {
    " ", "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83",
    "\xe2\x96\x84", "\xe2\x96\x85", "\xe2\x96\x86",
    "\xe2\x96\x87", "\xe2\x96\x88"
};

static void draw_history_chart(int top_y, int total_w, int chart_h) {
    int n = history_count < HISTORY_LEN ? history_count : HISTORY_LEN;
    if (n == 0) return;

    /* Layout: y-axis label (5) + border (1) + chart + border (1) + x-axis */
    int label_w = 5;
    int bx      = label_w + 1;           /* chart inner left x  */
    int ex      = total_w - 2;           /* chart inner right x */
    int avail_w = ex - bx;
    if (avail_w < 10) return;

    int inner_top = top_y + 1;           /* first data row (inside border) */
    int inner_bot = top_y + chart_h;     /* last  data row (inside border) */
    int bot_y     = top_y + chart_h + 1; /* bottom border row */
    int xaxis_y   = bot_y + 1;           /* time labels row   */

    /* ── Title ──────────────────────────────────────────────────────── */
    attron(COLOR_PAIR(8));
    mvprintw(top_y - 1, bx, "history  last %ds  ", HISTORY_LEN * delay_ms / 1000);
    attroff(COLOR_PAIR(8));
    attron(A_BOLD | COLOR_PAIR(2));
    printw("CPU");
    attroff(A_BOLD | COLOR_PAIR(2));
    attron(COLOR_PAIR(8));
    printw(" / ");
    attroff(COLOR_PAIR(8));
    attron(A_BOLD | COLOR_PAIR(6));
    printw("HPU");
    attroff(A_BOLD | COLOR_PAIR(6));

    /* ── Box border ─────────────────────────────────────────────────── */
    attron(COLOR_PAIR(8));
    /* Top border */
    mvaddch(top_y, bx - 1, ACS_ULCORNER);
    mvhline(top_y, bx, ACS_HLINE, avail_w);
    mvaddch(top_y, ex, ACS_URCORNER);
    /* Side borders */
    for (int r = inner_top; r <= inner_bot; r++) {
        mvaddch(r, bx - 1, ACS_VLINE);
        mvaddch(r, ex,     ACS_VLINE);
    }
    /* Bottom border */
    mvaddch(bot_y, bx - 1, ACS_LLCORNER);
    mvhline(bot_y, bx, ACS_HLINE, avail_w);
    mvaddch(bot_y, ex, ACS_LRCORNER);
    attroff(COLOR_PAIR(8));

    /* ── Y-axis labels and gridlines ────────────────────────────────── */
    struct { int pct; } grids[] = {{100},{75},{50},{25},{0}};
    for (int g = 0; g < 5; g++) {
        int pct = grids[g].pct;
        /* Row within chart: pct=100 → inner_top, pct=0 → inner_bot */
        int gy = inner_bot - (int)((double)pct / 100.0 * (inner_bot - inner_top) + 0.5);

        /* Y-axis label */
        attron(COLOR_PAIR(8));
        mvprintw(gy, 0, "%3d%%", pct);
        attroff(COLOR_PAIR(8));

        /* Dashed gridline inside box (skip 0% and 100% — those are borders) */
        if (pct > 0 && pct < 100) {
            attron(COLOR_PAIR(8) | A_DIM);
            for (int x = bx; x < ex; x += 2)
                mvaddch(gy, x, ACS_HLINE);
            attroff(COLOR_PAIR(8) | A_DIM);
        }
    }

    /* ── Data columns ───────────────────────────────────────────────── */
    int col_w   = avail_w / HISTORY_LEN;
    if (col_w < 1) col_w = 1;
    int x_start = bx + (avail_w - HISTORY_LEN * col_w); /* right-align */

    for (int s = 0; s < n; s++) {
        int idx     = (history_pos - n + s + HISTORY_LEN) % HISTORY_LEN;
        double cpu_val = cpu_history[idx];
        double hpu_val = hpu_history[idx];

        int cpu_blocks = (int)(cpu_val / 100.0 * (inner_bot - inner_top + 1) * 8 + 0.5);
        int hpu_blocks = (int)(hpu_val / 100.0 * (inner_bot - inner_top + 1) * 8 + 0.5);

        int x = x_start + s * col_w;
        if (x < bx || x >= ex) continue;
        int w = col_w;
        if (x + w > ex) w = ex - x;

        for (int row = 0; row < (inner_bot - inner_top + 1); row++) {
            int ry       = inner_bot - row;
            int row_base = row * 8;

            int cpu_fill = cpu_blocks - row_base;
            if (cpu_fill < 0) cpu_fill = 0;
            if (cpu_fill > 8) cpu_fill = 8;

            int hpu_fill = hpu_blocks - row_base;
            if (hpu_fill < 0) hpu_fill = 0;
            if (hpu_fill > 8) hpu_fill = 8;

            move(ry, x);
            /* HPU drawn on top of CPU — whichever is higher wins per cell */
            if (hpu_fill > 0) {
                attron(COLOR_PAIR(6));
                for (int c = 0; c < w; c++) addstr(block_chars[hpu_fill]);
                attroff(COLOR_PAIR(6));
            } else if (cpu_fill > 0) {
                attron(COLOR_PAIR(2));
                for (int c = 0; c < w; c++) addstr(block_chars[cpu_fill]);
                attroff(COLOR_PAIR(2));
            }
        }
    }

    /* ── X-axis time labels ─────────────────────────────────────────── */
    attron(COLOR_PAIR(8));
    for (int t = 0; t < HISTORY_LEN; t += 5) {
        int s = HISTORY_LEN - 1 - t;
        int x = x_start + s * col_w;
        if (x >= bx && x < ex - 3) {
            int secs = t * delay_ms / 1000;
            if (secs == 0)
                mvprintw(xaxis_y, x, "now");
            else
                mvprintw(xaxis_y, x, "-%ds", secs);
        }
    }
    attroff(COLOR_PAIR(8));
}

static void record_history(double cpu, double hpu) {
    cpu_history[history_pos] = cpu;
    hpu_history[history_pos] = hpu;
    history_pos = (history_pos + 1) % HISTORY_LEN;
    if (history_count < HISTORY_LEN) history_count++;
}

/* ── CSV logging ────────────────────────────────────────────────────── */

static void log_csv_header(FILE *f) {
    fprintf(f, "timestamp,cpu_avg_pct");
    for (int i = 1; i <= num_cpus; i++)
        fprintf(f, ",cpu%d_pct", i - 1);
    fprintf(f, ",cpu_temp_c,cpu_freq_mhz");
    fprintf(f, ",mem_used_kb,mem_total_kb,mem_bufcache_kb");
    fprintf(f, ",swap_used_kb,swap_total_kb");
    for (unsigned int g = 0; g < hpu_count; g++)
        fprintf(f, ",hpu%u_util_pct,hpu%u_temp_c,hpu%u_power_mw,hpu%u_clock_aic_mhz", g, g, g, g);
    fprintf(f, "\n");
    fflush(f);
}

static void log_csv_row(FILE *f) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fprintf(f, "%04d-%02d-%02dT%02d:%02d:%02d.%03ld",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);

    fprintf(f, ",%.1f", cpu_pct[0]);
    for (int i = 1; i <= num_cpus; i++)
        fprintf(f, ",%.1f", cpu_pct[i]);

    fprintf(f, ",%d,%d", read_cpu_temp(), read_cpu_freq_mhz());

    MemInfo mi;
    read_meminfo(&mi);
    fprintf(f, ",%llu,%llu,%llu", mi.app_kb, mi.total_kb, mi.bufcache_kb);
    fprintf(f, ",%llu,%llu", mi.swap_used_kb, mi.swap_total_kb);

    for (unsigned int g = 0; g < hpu_count; g++) {
        hlmlDevice_t dev;
        if (pHlmlDeviceGetHandleByIndex(g, &dev) == HLML_SUCCESS) {
            hlmlUtilization_t util = {0};
            pHlmlDeviceGetUtilizationRates(dev, &util);

            unsigned int temp = 0;
            pHlmlDeviceGetTemperature(dev, HLML_TEMPERATURE_AIP, &temp);

            unsigned int power_mw = 0;
            if (pHlmlDeviceGetPowerUsage)
                pHlmlDeviceGetPowerUsage(dev, &power_mw);

            unsigned int clk = 0;
            if (pHlmlDeviceGetClockInfo)
                pHlmlDeviceGetClockInfo(dev, HLML_CLOCK_AIC, &clk);

            fprintf(f, ",%u,%u,%u,%u", util.aip, temp, power_mw, clk);
        } else {
            fprintf(f, ",,,,");
        }
    }
    if (hpu_count == 0) fprintf(f, ",,,,");

    fprintf(f, "\n");
    fflush(f);
}

/* ── Prometheus metrics exporter ────────────────────────────────────── */

static int      prom_sock = -1;
static pthread_t prom_thread;

#define PROM_BUF_SIZE 65536
#define PROM_MAX_HPUS 8

typedef struct {
    int      valid;
    char     name[96];
    unsigned int util_aip;
    unsigned int temp;
    unsigned int power_mw;
    int      has_power;
    unsigned int clk_aic, clk_mme;
    unsigned long long mem_total, mem_used;
    int      has_mem;
} PromHpu;

static int format_metrics(char *buf, int buflen) {
    int off = 0;

    #define PM(...) do { \
        int _n = snprintf(buf + off, (size_t)(buflen - off), __VA_ARGS__); \
        if (_n > 0) { \
            if (_n >= buflen - off) { off = buflen - 1; goto pm_done; } \
            off += _n; \
        } \
    } while(0)

    PM("# HELP gaudi_build_info gaudi-monitor version\n"
       "# TYPE gaudi_build_info gauge\n"
       "gaudi_build_info{version=\"%s\"} 1\n", VERSION);

    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        PM("# HELP gaudi_uptime_seconds System uptime\n"
           "# TYPE gaudi_uptime_seconds gauge\n"
           "gaudi_uptime_seconds %ld\n", si.uptime);
    }

    double l1 = 0, l5 = 0, l15 = 0;
    get_loadavg(&l1, &l5, &l15);
    PM("# HELP gaudi_load_average System load average\n"
       "# TYPE gaudi_load_average gauge\n"
       "gaudi_load_average{interval=\"1m\"} %.2f\n"
       "gaudi_load_average{interval=\"5m\"} %.2f\n"
       "gaudi_load_average{interval=\"15m\"} %.2f\n", l1, l5, l15);

    PM("# HELP gaudi_cpu_usage_percent CPU utilization\n"
       "# TYPE gaudi_cpu_usage_percent gauge\n"
       "gaudi_cpu_usage_percent{cpu=\"overall\"} %.1f\n", cpu_pct[0]);
    for (int i = 1; i <= num_cpus; i++) {
        const char *lbl = cpu_part_label(i - 1);
        if (lbl[0])
            PM("gaudi_cpu_usage_percent{cpu=\"%d\",type=\"%s\"} %.1f\n",
               i - 1, lbl, cpu_pct[i]);
        else
            PM("gaudi_cpu_usage_percent{cpu=\"%d\"} %.1f\n", i - 1, cpu_pct[i]);
    }

    PM("# HELP gaudi_cpu_temperature_celsius CPU temperature\n"
       "# TYPE gaudi_cpu_temperature_celsius gauge\n"
       "gaudi_cpu_temperature_celsius %d\n", read_cpu_temp());

    PM("# HELP gaudi_cpu_frequency_mhz CPU frequency\n"
       "# TYPE gaudi_cpu_frequency_mhz gauge\n"
       "gaudi_cpu_frequency_mhz %d\n", read_cpu_freq_mhz());

    MemInfo mi;
    read_meminfo(&mi);
    PM("# HELP gaudi_memory_total_bytes Total system memory\n"
       "# TYPE gaudi_memory_total_bytes gauge\n"
       "gaudi_memory_total_bytes %llu\n"
       "# HELP gaudi_memory_used_bytes Application memory used\n"
       "# TYPE gaudi_memory_used_bytes gauge\n"
       "gaudi_memory_used_bytes %llu\n"
       "# HELP gaudi_memory_bufcache_bytes Buffer and cache memory\n"
       "# TYPE gaudi_memory_bufcache_bytes gauge\n"
       "gaudi_memory_bufcache_bytes %llu\n",
       mi.total_kb * 1024ULL, mi.app_kb * 1024ULL, mi.bufcache_kb * 1024ULL);

    if (mi.swap_total_kb > 0) {
        PM("# HELP gaudi_swap_total_bytes Total swap\n"
           "# TYPE gaudi_swap_total_bytes gauge\n"
           "gaudi_swap_total_bytes %llu\n"
           "# HELP gaudi_swap_used_bytes Swap used\n"
           "# TYPE gaudi_swap_used_bytes gauge\n"
           "gaudi_swap_used_bytes %llu\n",
           mi.swap_total_kb * 1024ULL, mi.swap_used_kb * 1024ULL);
    }

    /* HPU metrics */
    PromHpu hpus[PROM_MAX_HPUS];
    int n_hpus = 0;

    if (hlml_ok) {
        unsigned int dev_count = 0;
        pHlmlDeviceGetCount(&dev_count);

        for (unsigned int d = 0; d < dev_count && (int)d < PROM_MAX_HPUS; d++) {
            PromHpu *g = &hpus[n_hpus];
            memset(g, 0, sizeof(*g));
            hlmlDevice_t dev;
            if (pHlmlDeviceGetHandleByIndex(d, &dev) != HLML_SUCCESS) continue;
            g->valid = 1;
            pHlmlDeviceGetName(dev, g->name, sizeof(g->name));

            hlmlUtilization_t util = {0};
            pHlmlDeviceGetUtilizationRates(dev, &util);
            g->util_aip = util.aip;

            pHlmlDeviceGetTemperature(dev, HLML_TEMPERATURE_AIP, &g->temp);

            g->has_power = (pHlmlDeviceGetPowerUsage &&
                            pHlmlDeviceGetPowerUsage(dev, &g->power_mw) == HLML_SUCCESS);

            if (pHlmlDeviceGetClockInfo) {
                pHlmlDeviceGetClockInfo(dev, HLML_CLOCK_AIC, &g->clk_aic);
                pHlmlDeviceGetClockInfo(dev, HLML_CLOCK_MME, &g->clk_mme);
            }

            hlmlMemory_t mem = {0};
            g->has_mem = (pHlmlDeviceGetMemoryInfo &&
                          pHlmlDeviceGetMemoryInfo(dev, &mem) == HLML_SUCCESS &&
                          mem.total > 0);
            if (g->has_mem) { g->mem_total = mem.total; g->mem_used = mem.used; }

            n_hpus++;
        }
    }

    if (n_hpus > 0) {
        PM("# HELP gaudi_hpu_info HPU device information\n"
           "# TYPE gaudi_hpu_info gauge\n");
        for (int d = 0; d < n_hpus; d++)
            PM("gaudi_hpu_info{hpu=\"%d\",name=\"%s\"} 1\n", d, hpus[d].name);

        PM("# HELP gaudi_hpu_utilization_percent HPU AI core utilization\n"
           "# TYPE gaudi_hpu_utilization_percent gauge\n");
        for (int d = 0; d < n_hpus; d++)
            PM("gaudi_hpu_utilization_percent{hpu=\"%d\"} %u\n", d, hpus[d].util_aip);

        PM("# HELP gaudi_hpu_temperature_celsius HPU die temperature\n"
           "# TYPE gaudi_hpu_temperature_celsius gauge\n");
        for (int d = 0; d < n_hpus; d++)
            PM("gaudi_hpu_temperature_celsius{hpu=\"%d\"} %u\n", d, hpus[d].temp);

        PM("# HELP gaudi_hpu_power_watts HPU power draw\n"
           "# TYPE gaudi_hpu_power_watts gauge\n");
        for (int d = 0; d < n_hpus; d++)
            if (hpus[d].has_power)
                PM("gaudi_hpu_power_watts{hpu=\"%d\"} %.1f\n", d, hpus[d].power_mw / 1000.0);

        PM("# HELP gaudi_hpu_clock_mhz HPU clock speed\n"
           "# TYPE gaudi_hpu_clock_mhz gauge\n");
        for (int d = 0; d < n_hpus; d++) {
            if (hpus[d].clk_aic)
                PM("gaudi_hpu_clock_mhz{hpu=\"%d\",type=\"aic\"} %u\n", d, hpus[d].clk_aic);
            if (hpus[d].clk_mme)
                PM("gaudi_hpu_clock_mhz{hpu=\"%d\",type=\"mme\"} %u\n", d, hpus[d].clk_mme);
        }

        PM("# HELP gaudi_hpu_memory_total_bytes HPU HBM memory total\n"
           "# TYPE gaudi_hpu_memory_total_bytes gauge\n");
        for (int d = 0; d < n_hpus; d++)
            if (hpus[d].has_mem)
                PM("gaudi_hpu_memory_total_bytes{hpu=\"%d\"} %llu\n", d, hpus[d].mem_total);

        PM("# HELP gaudi_hpu_memory_used_bytes HPU HBM memory used\n"
           "# TYPE gaudi_hpu_memory_used_bytes gauge\n");
        for (int d = 0; d < n_hpus; d++)
            if (hpus[d].has_mem)
                PM("gaudi_hpu_memory_used_bytes{hpu=\"%d\"} %llu\n", d, hpus[d].mem_used);
    }

pm_done:
    #undef PM
    return off;
}

static void prom_handle(int fd) {
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req[512];
    int n = (int)recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0) return;
    req[n] = '\0';

    if (prom_token) {
        char expected[512];
        snprintf(expected, sizeof(expected), "Authorization: Bearer %s", prom_token);
        if (!strstr(req, expected)) {
            static const char resp_401[] =
                "HTTP/1.1 401 Unauthorized\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n\r\n"
                "Unauthorized\n";
            send(fd, resp_401, sizeof(resp_401) - 1, MSG_NOSIGNAL);
            return;
        }
    }

    if (strstr(req, "GET /metrics")) {
        char body[PROM_BUF_SIZE];
        int bodylen = format_metrics(body, sizeof(body));

        char hdr[128];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n", bodylen);

        send(fd, hdr, hlen, MSG_NOSIGNAL);
        send(fd, body, bodylen, MSG_NOSIGNAL);
    } else {
        static const char resp[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n\r\n"
            "<html><body><h1>gaudi-monitor</h1>"
            "<p><a href=\"/metrics\">Metrics</a></p>"
            "</body></html>\n";
        send(fd, resp, sizeof(resp) - 1, MSG_NOSIGNAL);
    }
}

static void *prom_server(void *arg) {
    (void)arg;
    while (!g_quit) {
        struct pollfd pfd = { .fd = prom_sock, .events = POLLIN };
        if (poll(&pfd, 1, 1000) <= 0) continue;

        int fd = accept(prom_sock, NULL, NULL);
        if (fd < 0) continue;
        prom_handle(fd);
        close(fd);
    }
    return NULL;
}

static int prom_start(void) {
    if (!prom_port) return 0;

    prom_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (prom_sock < 0) { perror("prometheus: socket"); return -1; }

    int opt = 1;
    setsockopt(prom_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)prom_port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(prom_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("prometheus: bind");
        close(prom_sock); prom_sock = -1; return -1;
    }
    if (listen(prom_sock, 4) < 0) {
        perror("prometheus: listen");
        close(prom_sock); prom_sock = -1; return -1;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 131072);

    if (pthread_create(&prom_thread, &attr, prom_server, NULL) != 0) {
        perror("prometheus: pthread_create");
        close(prom_sock); prom_sock = -1;
        pthread_attr_destroy(&attr); return -1;
    }
    pthread_attr_destroy(&attr);
    fprintf(stderr, "Prometheus metrics at http://0.0.0.0:%d/metrics\n", prom_port);
    return 0;
}

static void prom_stop(void) {
    if (prom_sock >= 0) {
        pthread_join(prom_thread, NULL);
        close(prom_sock);
        prom_sock = -1;
    }
}

/* ── Usage ──────────────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  -l FILE   Log statistics to CSV file\n"
        "  -i MS     Log interval in milliseconds (default: 1000)\n"
        "  -n        No UI (headless mode, requires -l or -p)\n"
        "  -p PORT   Expose Prometheus metrics on PORT\n"
        "  -t TOKEN  Require Bearer token for /metrics (or GAUDI_MONITOR_TOKEN env)\n"
        "  -r MS     UI refresh interval in milliseconds (default: 1000)\n"
        "  -v        Show version\n"
        "  -h        Show this help\n"
        "\n"
        "Examples:\n"
        "  %s -l stats.csv                  TUI + logging every 1s\n"
        "  %s -l stats.csv -i 5000          TUI + logging every 5s\n"
        "  %s -n -l stats.csv -i 500        Headless, log every 500ms\n"
        "  %s -r 2000                       TUI refreshing every 2s\n"
        "\n"
        "https://github.com/your-org/gaudi-monitor\n",
        prog, prog, prog, prog, prog);
}

/* ── Main draw ──────────────────────────────────────────────────────── */

static void draw_screen(void) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();
    int y = 0;

    /* ── Header ─────────────────────────────────────────────────────── */
    attron(A_BOLD | COLOR_PAIR(6));
    mvprintw(y, 0, " gaudi-monitor");
    attroff(A_BOLD | COLOR_PAIR(6));
    attron(COLOR_PAIR(8));
    printw(" |");
    attroff(COLOR_PAIR(8));
    attron(COLOR_PAIR(7));
    printw(" %s", cpu_model_name[0] ? cpu_model_name : "Unknown CPU");
    attroff(COLOR_PAIR(7));

    char upbuf[64];
    fmt_uptime(upbuf, sizeof(upbuf));
    double l1, l5, l15;
    l1 = l5 = l15 = 0;
    get_loadavg(&l1, &l5, &l15);
    {
        /* Current time */
        time_t now = time(NULL);
        struct tm *tm_now = localtime(&now);
        char timebuf[16];
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_now);

        char info[160];
        int len = snprintf(info, sizeof(info), "%s  up %s  load %.2f %.2f %.2f",
                           timebuf, upbuf, l1, l5, l15);
        attron(COLOR_PAIR(8));
        mvprintw(y, cols - len - 1, "%s", info);
        attroff(COLOR_PAIR(8));
    }
    y++;

    attron(COLOR_PAIR(8));
    mvhline(y, 0, ACS_HLINE, cols);
    attroff(COLOR_PAIR(8));
    attron(A_BOLD | COLOR_PAIR(3));
    mvprintw(y, 2, " CPU ");
    attroff(A_BOLD | COLOR_PAIR(3));
    attron(COLOR_PAIR(4));
    mvprintw(y, cols / 2 - 4, " MEM ");
    attroff(COLOR_PAIR(4));
    y++;

    /* ── CPU section ────────────────────────────────────────────────── */
    int cpu_temp = read_cpu_temp();
    int cpu_freq = read_cpu_freq_mhz();

    attron(A_BOLD | COLOR_PAIR(3));
    mvprintw(y, 1, "CPU");
    attroff(A_BOLD | COLOR_PAIR(3));
    attron(COLOR_PAIR(8));
    printw("  %d cores", num_cpus);
    if (cpu_freq > 0) printw("  %d MHz", cpu_freq);
    attroff(COLOR_PAIR(8));
    if (cpu_temp > 0) {
        int tc = cpu_temp > 80 ? 1 : (cpu_temp > 60 ? 3 : 8);
        attron(COLOR_PAIR(tc));
        printw("  %d C", cpu_temp);
        attroff(COLOR_PAIR(tc));
    }

    /* Overall bar — right side of header line */
    {
        attron(A_BOLD | COLOR_PAIR(7));
        mvprintw(y, cols - 26, "Overall:");
        attroff(A_BOLD | COLOR_PAIR(7));
        int bx = cols - 17;
        int bw = 12;
        int color = cpu_pct[0] > 90 ? 1 : (cpu_pct[0] > 60 ? 3 : 2);
        draw_bar(y, bx, bw, cpu_pct[0], color);
        attron(COLOR_PAIR(color) | A_BOLD);
        mvprintw(y, bx + bw + 2, "%4.1f%%", cpu_pct[0]);
        attroff(COLOR_PAIR(color) | A_BOLD);
    }
    y++;

    /* ── Fixed 20-core display, two columns of 10, bars scale to width ─
       Always shows cores 0–19. Values update live, layout never shifts. */
    {
        int n_show  = 20;
        if (n_show > num_cpus) n_show = num_cpus;
        int half    = n_show / 2;          /* 10 left, 10 right */
        int lbl_w   = 5;                   /* "  0  " */
        int mid     = cols / 2;
        int bar_w   = mid - lbl_w - 9;    /* left bar width  */
        int bar_wr  = cols - (mid + lbl_w + 1) - 9; /* right bar width */
        if (bar_w  < 5) bar_w  = 5;
        if (bar_wr < 5) bar_wr = 5;

        for (int i = 0; i < half; i++) {
            int cl = i;
            int cr = i + half;

            /* Left core */
            int lcolor = cpu_pct[cl + 1] > 90 ? 1 : (cpu_pct[cl + 1] > 60 ? 3 : 2);
            attron(COLOR_PAIR(8));
            mvprintw(y, 1, "%3d", cl);
            attroff(COLOR_PAIR(8));
            draw_bar(y, 1 + lbl_w, bar_w, cpu_pct[cl + 1], lcolor);
            attron(COLOR_PAIR(lcolor));
            mvprintw(y, 1 + lbl_w + bar_w + 2, "%4.1f%%", cpu_pct[cl + 1]);
            attroff(COLOR_PAIR(lcolor));

            /* Right core */
            if (cr < num_cpus) {
                int rcolor = cpu_pct[cr + 1] > 90 ? 1 : (cpu_pct[cr + 1] > 60 ? 3 : 2);
                attron(COLOR_PAIR(8));
                mvprintw(y, mid + 1, "%3d", cr);
                attroff(COLOR_PAIR(8));
                draw_bar(y, mid + 1 + lbl_w, bar_wr, cpu_pct[cr + 1], rcolor);
                attron(COLOR_PAIR(rcolor));
                mvprintw(y, mid + 1 + lbl_w + bar_wr + 2, "%4.1f%%", cpu_pct[cr + 1]);
                attroff(COLOR_PAIR(rcolor));
            }
            y++;
        }

        /* Active core count summary */
        int n_active = 0;
        for (int i = 0; i < num_cpus; i++)
            if (cpu_pct[i + 1] > 0.0) n_active++;
        attron(COLOR_PAIR(8));
        mvprintw(y, 1, "%d/%d cores active  (showing 0-%d)",
                 n_active, num_cpus, n_show - 1);
        attroff(COLOR_PAIR(8));
        y++;
    }
    y++;

    /* ── Memory section ─────────────────────────────────────────────── */
    MemInfo mi;
    read_meminfo(&mi);
    double pct_app      = mi.total_kb ? (double)mi.app_kb      / mi.total_kb * 100.0 : 0;
    double pct_bufcache = mi.total_kb ? (double)mi.bufcache_kb / mi.total_kb * 100.0 : 0;

    attron(A_BOLD | COLOR_PAIR(4));
    mvprintw(y, 1, "MEM");
    attroff(A_BOLD | COLOR_PAIR(4));

    char tb[16], ab[16], bb[16];
    fmt_bytes(mi.total_kb * 1024ULL, tb, sizeof(tb));
    fmt_bytes(mi.app_kb   * 1024ULL, ab, sizeof(ab));
    fmt_bytes(mi.bufcache_kb * 1024ULL, bb, sizeof(bb));
    printw("  ");
    attron(COLOR_PAIR(2));
    printw("%s used", ab);
    attroff(COLOR_PAIR(2));
    printw(" + ");
    attron(COLOR_PAIR(4));
    printw("%s buf/cache", bb);
    attroff(COLOR_PAIR(4));
    printw(" / %s", tb);
    y++;

    {
        int bw = cols - 13;
        if (bw < 10) bw = 10;
        draw_bar_segmented(y, 4, bw, pct_app, pct_bufcache, 2, 4);
        mvprintw(y, 4 + bw + 2, " %.1f%%", pct_app + pct_bufcache);
    }
    y++;

    if (mi.swap_total_kb > 0) {
        double swap_pct = (double)mi.swap_used_kb / mi.swap_total_kb * 100.0;
        attron(A_BOLD | COLOR_PAIR(4));
        mvprintw(y, 1, "SWP");
        attroff(A_BOLD | COLOR_PAIR(4));
        char stb[16], sub[16];
        fmt_bytes(mi.swap_used_kb   * 1024ULL, sub, sizeof(sub));
        fmt_bytes(mi.swap_total_kb  * 1024ULL, stb, sizeof(stb));
        printw("  %s / %s", sub, stb);
        y++;
        {
            int bw = cols - 13;
            if (bw < 10) bw = 10;
            int color = swap_pct > 80 ? 1 : (swap_pct > 40 ? 3 : 5);
            draw_bar(y, 4, bw, swap_pct, color);
            attron(COLOR_PAIR(color));
            mvprintw(y, 4 + bw + 2, " %.1f%%", swap_pct);
            attroff(COLOR_PAIR(color));
        }
        y++;
    }

    /* ── HPU section label/separator ───────────────────────────────── */
    attron(COLOR_PAIR(8));
    mvhline(y, 0, ACS_HLINE, cols);
    attroff(COLOR_PAIR(8));
    attron(A_BOLD | COLOR_PAIR(6));
    mvprintw(y, 2, " HPU ");
    attroff(A_BOLD | COLOR_PAIR(6));
    attron(COLOR_PAIR(8));
    {
        char hpu_info[64];
        snprintf(hpu_info, sizeof(hpu_info), " %u x HL-325L (Gaudi 3) ", hpu_count);
        printw("%s", hpu_info);
    }
    attroff(COLOR_PAIR(8));
    y++;

    /* ── HPU section ────────────────────────────────────────────────── */
    if (!hlml_ok) {
        attron(COLOR_PAIR(1));
        mvprintw(y, 1, "HPU: HLML not available");
        attroff(COLOR_PAIR(1));

        /* Sysfs fallback: show detected Gaudi devices without metrics */
        if (sysfs_hpu_count > 0) {
            attron(COLOR_PAIR(7));
            mvprintw(y + 1, 1, "  Detected %d Gaudi device(s) via /sys/class/habanalabs",
                     sysfs_hpu_count);
            attroff(COLOR_PAIR(7));
            for (int i = 0; i < sysfs_hpu_count && y + 2 + i < rows - 4; i++) {
                attron(COLOR_PAIR(8));
                mvprintw(y + 2 + i, 3, "HPU %d: %s", i, sysfs_hpu_names[i]);
                attroff(COLOR_PAIR(8));
            }
        }
        y += 3;
    } else {
        double hpu_util_sum = 0;
        unsigned int hpu_util_n = 0;

        for (unsigned int d = 0; d < hpu_count && y < rows - 4; d++) {
            hlmlDevice_t dev;
            if (pHlmlDeviceGetHandleByIndex(d, &dev) != HLML_SUCCESS) continue;

            char name[96] = "Gaudi HPU";
            pHlmlDeviceGetName(dev, name, sizeof(name));

            hlmlUtilization_t util = {0};
            pHlmlDeviceGetUtilizationRates(dev, &util);
            hpu_util_sum += (double)util.aip;
            hpu_util_n++;

            unsigned int temp = 0;
            pHlmlDeviceGetTemperature(dev, HLML_TEMPERATURE_AIP, &temp);

            unsigned int power_mw = 0;
            int has_power = (pHlmlDeviceGetPowerUsage &&
                             pHlmlDeviceGetPowerUsage(dev, &power_mw) == HLML_SUCCESS);

            unsigned int clk_aic = 0;
            if (pHlmlDeviceGetClockInfo)
                pHlmlDeviceGetClockInfo(dev, HLML_CLOCK_AIC, &clk_aic);

            /* HPU header line with inline AIP bar */
            attron(A_BOLD | COLOR_PAIR(6));
            mvprintw(y, 1, "HPU %u", d);
            attroff(A_BOLD | COLOR_PAIR(6));
            attron(COLOR_PAIR(7));
            printw("  %s", name);
            attroff(COLOR_PAIR(7));
            {
                int temp_color = temp > 80 ? 1 : (temp > 60 ? 3 : 8);
                attron(COLOR_PAIR(temp_color));
                printw("  %u C", temp);
                attroff(COLOR_PAIR(temp_color));
            }
            if (has_power) {
                attron(COLOR_PAIR(8));
                printw("  %.1fW", power_mw / 1000.0);
                attroff(COLOR_PAIR(8));
            }
            if (clk_aic) {
                attron(COLOR_PAIR(8));
                printw("  %u MHz", clk_aic);
                attroff(COLOR_PAIR(8));
            }
            /* AIP bar inline — fill remaining width */
            {
                int cur_x = getcurx(stdscr);
                attron(COLOR_PAIR(8));
                printw("  AIP ");
                attroff(COLOR_PAIR(8));
                int bx = getcurx(stdscr);
                int bw = cols - bx - 6; /* 6 for " 100% " */
                if (bw > 80) bw = 80;
                if (bw < 5)  bw = 5;
                int color = util.aip > 90 ? 1 : (util.aip > 60 ? 3 : 6);
                draw_bar(y, bx, bw, (double)util.aip, color);
                attron(COLOR_PAIR(color) | A_BOLD);
                mvprintw(y, bx + bw + 2, " %3u%%", util.aip);
                attroff(COLOR_PAIR(color) | A_BOLD);
                (void)cur_x;
            }
            y++;

            /* HBM memory usage */
            hlmlMemory_t mem = {0};
            int has_mem = (pHlmlDeviceGetMemoryInfo &&
                           pHlmlDeviceGetMemoryInfo(dev, &mem) == HLML_SUCCESS &&
                           mem.total > 0);

            if (has_mem) {
                attron(COLOR_PAIR(8));
                mvprintw(y, 1, "  HBM ");
                attroff(COLOR_PAIR(8));
                int bx = 7;
                int bw = cols - bx - 22;
                if (bw < 10) bw = 10;
                double mem_pct = (double)mem.used / mem.total * 100.0;
                int color = mem_pct > 90 ? 1 : (mem_pct > 60 ? 3 : 5);
                draw_bar(y, bx, bw, mem_pct, color);
                char ub[16], tb2[16];
                fmt_bytes(mem.used,  ub,  sizeof(ub));
                fmt_bytes(mem.total, tb2, sizeof(tb2));
                attron(COLOR_PAIR(8));
                mvprintw(y, bx + bw + 2 + 1, "%s/", ub);
                attroff(COLOR_PAIR(8));
                printw("%s", tb2);
                attron(COLOR_PAIR(color));
                printw(" %3.0f%%", mem_pct);
                attroff(COLOR_PAIR(color));
            } else {
                attron(COLOR_PAIR(8));
                mvprintw(y, 1, "  HBM");
                attroff(COLOR_PAIR(8));
                attron(COLOR_PAIR(7));
                printw("  no memory info available");
                attroff(COLOR_PAIR(7));
            }
            y++;

            /* HPU processes */
            hlmlProcessInfo_t procs_raw[MAX_HPU_PROCS];
            unsigned int n_procs = MAX_HPU_PROCS;
            HpuProc all_procs[MAX_HPU_PROCS];
            int n_all = 0;

            if (pHlmlDeviceGetComputeRunningProcesses)
                pHlmlDeviceGetComputeRunningProcesses(dev, &n_procs, procs_raw);
            else
                n_procs = 0;

            for (unsigned int i = 0; i < n_procs && n_all < MAX_HPU_PROCS; i++) {
                HpuProc *p = &all_procs[n_all++];
                p->pid = procs_raw[i].pid;
                p->mem_bytes = procs_raw[i].usedHlMemory;
                p->cpu_pct   = calc_proc_cpu_pct(p->pid);
                get_proc_cmdline(p->pid, p->name, sizeof(p->name));
                get_proc_user(p->pid, p->user, sizeof(p->user));
            }

            update_proc_cpu_snapshots(all_procs, n_all);

            /* Sort */
            for (int i = 0; i < n_all - 1; i++)
                for (int j = i + 1; j < n_all; j++) {
                    int swap_flag = 0;
                    if (sort_mode == 0)
                        swap_flag = all_procs[j].mem_bytes > all_procs[i].mem_bytes;
                    else
                        swap_flag = all_procs[j].pid < all_procs[i].pid;
                    if (swap_flag) {
                        HpuProc tmp = all_procs[i];
                        all_procs[i] = all_procs[j];
                        all_procs[j] = tmp;
                    }
                }

            if (n_all > 0) {
                attron(A_BOLD | COLOR_PAIR(7));
                mvprintw(y, 1, "  %-8s %-12s %-4s %7s %-12s %s",
                         "PID", "USER", "TYPE", "CPU%", "HPU MEM", "COMMAND");
                attroff(A_BOLD | COLOR_PAIR(7));
                y++;

                for (int i = 0; i < n_all && y < rows - 2; i++) {
                    HpuProc *p = &all_procs[i];
                    char mb[16];
                    if (p->mem_bytes > 0)
                        fmt_bytes(p->mem_bytes, mb, sizeof(mb));
                    else
                        snprintf(mb, sizeof(mb), "N/A");

                    int name_max = cols - 54;
                    if (name_max < 10) name_max = 10;
                    char truncname[256];
                    snprintf(truncname, sizeof(truncname), "%-.*s", name_max, p->name);

                    mvprintw(y, 1, "  %-8u %-12s ", p->pid, p->user);
                    attron(COLOR_PAIR(5));
                    printw("%-4s ", "C");
                    attroff(COLOR_PAIR(5));
                    printw("%6.1f%% %-12s %s", p->cpu_pct, mb, truncname);
                    y++;
                }

                double hpu_proc_cpu = 0;
                for (int i = 0; i < n_all; i++)
                    hpu_proc_cpu += all_procs[i].cpu_pct;
                double total_cpu = cpu_pct[0] * num_cpus;
                double other_cpu = total_cpu - hpu_proc_cpu;
                if (other_cpu < 0) other_cpu = 0;
                attron(COLOR_PAIR(8));
                mvprintw(y, 1, "  %-8s %-12s %-4s %6.1f%% %-12s %s",
                         "", "", "", other_cpu, "", "(other processes)");
                attroff(COLOR_PAIR(8));
                y++;
            }

            /* Thin separator between HPU cards (not after last) */
            if (d + 1 < hpu_count && y < rows - 4) {
                attron(COLOR_PAIR(8));
                mvhline(y, 2, ACS_HLINE, cols - 4);
                attroff(COLOR_PAIR(8));
                y++;
            }
        }

        last_hpu_util = hpu_util_n > 0 ? hpu_util_sum / hpu_util_n : 0;
    }

    /* ── History chart ──────────────────────────────────────────────── */
    record_history(cpu_pct[0], last_hpu_util);
    {
        int chart_h = 10;
        y++;
        if (y + chart_h + 4 < rows && cols > 20) {
            draw_history_chart(y + 1, cols, chart_h);
            y += chart_h + 4; /* +1 title, +1 top border, +1 bot border, +1 xaxis */
        }
    }

    /* ── Footer ─────────────────────────────────────────────────────── */
    attron(COLOR_PAIR(8));
    mvhline(rows - 1, 0, ACS_HLINE, cols);
    attroff(COLOR_PAIR(8));
    move(rows - 1, 1);
    attron(A_BOLD | COLOR_PAIR(7));
    printw(" q");
    attroff(A_BOLD | COLOR_PAIR(7));
    printw(":quit ");
    attron(A_BOLD | COLOR_PAIR(7));
    printw("s");
    attroff(A_BOLD | COLOR_PAIR(7));
    printw(":sort ");
    attron(A_BOLD | COLOR_PAIR(7));
    printw("+/-");
    attroff(A_BOLD | COLOR_PAIR(7));
    printw(":speed  ");
    attron(COLOR_PAIR(8));
    printw("%.1fs", delay_ms / 1000.0);
    attroff(COLOR_PAIR(8));

    attron(COLOR_PAIR(8));
    mvprintw(rows - 1, cols - (int)strlen(VERSION) - 2, "%s ", VERSION);
    attroff(COLOR_PAIR(8));

    refresh();
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C"); /* Force decimal point for Prometheus exposition format */
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    const char *log_path = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "l:i:np:t:r:vh")) != -1) {
        switch (opt) {
        case 'l': log_path        = optarg;       break;
        case 'i': log_interval_ms = atoi(optarg); break;
        case 'n': no_ui           = 1;            break;
        case 'p': prom_port       = atoi(optarg); break;
        case 't': prom_token      = optarg;        break;
        case 'r': delay_ms        = atoi(optarg); break;
        case 'v': printf("gaudi-monitor %s\n", VERSION); return 0;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    if (!prom_token)
        prom_token = getenv("GAUDI_MONITOR_TOKEN");

    if (no_ui && !log_path && !prom_port) {
        fprintf(stderr, "Error: -n (no UI) requires -l <file> or -p <port>\n");
        return 1;
    }
    if (log_interval_ms < 100) log_interval_ms = 100;
    if (delay_ms < 250) delay_ms = 250;

    if (log_path) {
        log_fp = fopen(log_path, "w");
        if (!log_fp) { perror(log_path); return 1; }
    }

    /* Load HLML */
    hlml_ok = (load_hlml() == 0);
    if (hlml_ok && pHlmlDeviceGetCount)
        pHlmlDeviceGetCount(&hpu_count);

    /* Sysfs fallback for device discovery */
    if (!hlml_ok || hpu_count == 0)
        read_sysfs_hpu_names();

    read_cpu_model_name();
    read_cpu_part_ids();

    read_cpu_ticks(prev_ticks, &num_cpus);
    usleep(100000);
    compute_cpu_usage();

    if (log_fp) log_csv_header(log_fp);

    if (prom_port && prom_start() != 0) return 1;

    if (no_ui) {
        int headless_interval = log_fp ? log_interval_ms : delay_ms;
        if (log_fp)
            fprintf(stderr, "Logging to %s every %dms (Ctrl+C to stop)\n",
                    log_path, headless_interval);
        else
            fprintf(stderr, "Running headless (Ctrl+C to stop)\n");
        while (!g_quit) {
            compute_cpu_usage();
            if (log_fp) log_csv_row(log_fp);
            usleep(headless_interval * 1000);
        }
        fprintf(stderr, "\nStopped.\n");
    } else {
        initscr();
        cbreak();
        noecho();
        curs_set(0);
        nodelay(stdscr, TRUE);
        keypad(stdscr, TRUE);

        if (has_colors()) {
            start_color();
            use_default_colors();
            init_pair(1, COLOR_RED,     -1);
            init_pair(2, COLOR_GREEN,   -1);
            init_pair(3, COLOR_YELLOW,  -1);
            init_pair(4, COLOR_BLUE,    -1);
            init_pair(5, COLOR_MAGENTA, -1);
            init_pair(6, COLOR_CYAN,    -1);
            init_pair(7, COLOR_WHITE,   -1);
            init_pair(8, 244,           -1);
        }

        int log_elapsed = 0;

        while (!g_quit) {
            compute_cpu_usage();
            draw_screen();

            if (log_fp) {
                log_elapsed += delay_ms;
                if (log_elapsed >= log_interval_ms) {
                    log_csv_row(log_fp);
                    log_elapsed = 0;
                }
            }

            int elapsed = 0;
            while (elapsed < delay_ms && !g_quit) {
                int ch = getch();
                if (ch == 'q' || ch == 'Q' || ch == 27) {
                    g_quit = 1;
                    break;
                } else if (ch == 's' || ch == 'S') {
                    sort_mode = (sort_mode + 1) % 2;
                    break;
                } else if (ch == '+' || ch == '=') {
                    if (delay_ms > 250) delay_ms -= 250;
                } else if (ch == '-' || ch == '_') {
                    if (delay_ms < 5000) delay_ms += 250;
                } else if (ch == KEY_RESIZE) {
                    break;
                }
                usleep(50000);
                elapsed += 50;
            }
        }

        endwin();
    }

    prom_stop();
    if (log_fp) fclose(log_fp);
    if (hlml_ok && pHlmlShutdown) pHlmlShutdown();
    if (hlml_handle) dlclose(hlml_handle);

    return 0;
}
