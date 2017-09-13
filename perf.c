#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include <asm/unistd.h>

#define MAX 16
#define ASCII_OFFSET 48
//#define ENABLE_LOG
#define ENABLE_GETTIMEOFDAY

int cpus[MAX], fd[MAX], kCpu_num;

struct read_format {
    uint64_t value;
    uint64_t time_enabled;
    uint64_t time_running;
    uint64_t id;
};

bool isDigit(char num) {
    if (num >= '0' && num <= '9')
        return true;
    return false;
}

int get_online_cpus() {
    char* line = NULL;
    size_t len = 0;
    FILE* fp = fopen("/sys/devices/system/cpu/online", "re");
    int i;
    bool have_dash = false;
    kCpu_num = 0;

    if (fp == NULL) {
        printf("[Error] There is no online cpu.\n");
        return 0;
    }
    if (getline(&line, &len, fp) != -1) {
        for (i=0; i<len; i++) {
            if (line[i] == '\0')
                break;
            if (isDigit(line[i])) {
                cpus[kCpu_num] = (int)line[i]-ASCII_OFFSET;
#ifdef ENABLE_LOG
                printf("cpu[%d]=%d\n", kCpu_num, (int)line[i]-ASCII_OFFSET);
#endif
                kCpu_num++;
            } else if (line[i] == '-') {
                if (isDigit(line[i+1])) {
                    while (cpus[kCpu_num-1] < ((int)line[i+1]-ASCII_OFFSET)) {
                        cpus[kCpu_num] = cpus[kCpu_num-1] + 1;
#ifdef ENABLE_LOG
                        printf("cpu[%d]=%d\n", kCpu_num, cpus[kCpu_num-1] + 1);
#endif
                        kCpu_num++;
                    }
                    i++;
                }
            }
        }
    }

    free(line);
    return kCpu_num;
}

static long
perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                        int cpu, int group_fd, unsigned long flags)
{
    int ret;
    ret = syscall(__NR_perf_event_open, hw_event, pid, cpu,
            group_fd, flags);
    return ret;
}

struct perf_event_attr *alloc_perf_attr()
{
    struct perf_event_attr *pe = malloc(sizeof(struct perf_event_attr));

    memset(pe, 0, sizeof(struct perf_event_attr));
    pe->type = PERF_TYPE_HARDWARE;
    pe->size = sizeof(struct perf_event_attr);
    pe->config = PERF_COUNT_HW_CPU_CYCLES;
    pe->disabled = 1;
    pe->inherit = 1;
    pe->inherit_stat = 1;
    pe->mmap = 1;
    pe->comm = 1;
    pe->read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | 
        PERF_FORMAT_TOTAL_TIME_RUNNING | PERF_FORMAT_ID;

    return pe;
}

int perf_init_all_cpus(struct perf_event_attr *pe, int num_cpu)
{
    int i;
    for (i=0; i<num_cpu; i++) {
        fd[i] = perf_event_open(pe, 0, cpus[i], -1, 0);
        if (fd[i] == -1) {
            printf("[Error] Failed to allocate cpu%d\n", cpus[i]);
            return -1;
        }
    }

    for (i=0; i<num_cpu; i++) ioctl(fd[i], PERF_EVENT_IOC_RESET, 0);
    for (i=0; i<num_cpu; i++) ioctl(fd[i], PERF_EVENT_IOC_ENABLE, 0);
    return 0;
}

struct read_format *get_perf_result(int num_cpu)
{
    int i;
    for (i=0; i<num_cpu; i++) ioctl(fd[i], PERF_EVENT_IOC_DISABLE, 0);
    
    struct read_format *result = malloc(num_cpu*sizeof(struct read_format));
    for (i=0; i<num_cpu; i++) {
        read(fd[i], &result[i], sizeof(struct read_format));
#ifdef ENABLE_LOG
        printf(" cpu%d-cyles : count %lld, time_enaled %lld, time running %lld\n",
                cpus[i], result[i].value, result[i].time_enabled, 
                result[i].time_running);
#endif
        close(fd[i]);
    }

    return result;
}

uint64_t calc_all_cpus(struct read_format *result, int num_cpu)
{
    double scale = 0.0;
    long long sum_time_enabled=0, sum_time_running=0, sum_value=0;
    int i;

    for (i=0; i<num_cpu; i++) {
        sum_time_enabled += result[i].time_enabled;
        sum_time_running += result[i].time_running;
        sum_value += result[i].value;
#ifdef ENABLE_LOG
        printf("  cpu%d : count %lld, time_enabled %lld, time running %lld\n",
                cpus[i], result[i].value, result[i].time_enabled, 
                result[i].time_running);
#endif
    }

    if (sum_time_running !=0)
        scale = (double)(sum_time_enabled) / sum_time_running;

    return (uint64_t)(scale * sum_value);
}

int main(int argc, char **argv)
{
    struct perf_event_attr *pe;
    struct read_format *result;
    struct timeval start_time, end_time;
    long time_diff;
    uint64_t cpu_cycle;
    int opt = 0, status, duration=0;
    char* cmd;

    while ((opt = getopt(argc, argv, "t:x:h:")) != -1) {
        switch (opt) {
            case 't':
                duration = atoi(optarg);
                break;
            case 'x':
                cmd = (char *)malloc(strlen(optarg)+1);
                strcpy(cmd, optarg);
                break;
            case '?':
            case 'h':
            default:
            {
                printf("usage: %s [options] \"[workload command]\"\n", argv[0]);
                printf("       -h(elp)\n");
                exit(1);
                break;
            }
        }
    }

    kCpu_num = get_online_cpus();
    pe = alloc_perf_attr();
    perf_init_all_cpus(pe, kCpu_num);

#ifdef ENABLE_GETTIMEOFDAY
    gettimeofday(&start_time, NULL);
#endif
    status = system(cmd);
#ifdef ENABLE_GETTIMEOFDAY
    gettimeofday(&end_time, NULL);
#endif

    result = get_perf_result(kCpu_num);
    cpu_cycle = calc_all_cpus(result, kCpu_num);
    printf("     %lld  cpu-cycles\n", cpu_cycle);

#ifdef ENABLE_GETTIMEOFDAY
    time_diff = 1000000*(end_time.tv_sec - start_time.tv_sec) +
        (end_time.tv_usec - start_time.tv_usec);
    printf("     %ld.%ld - %ld.%ld\n", end_time.tv_sec, end_time.tv_usec,
            start_time.tv_sec, start_time.tv_usec);
    printf("     %ld.%ld secs time-diff\n", time_diff/1000000,
            time_diff%1000000);
#endif

    if (duration !=0)
        printf("     %lld MCPS\n", cpu_cycle/duration/1000000);

    free(pe);
    free(result);
    return 0;
}
