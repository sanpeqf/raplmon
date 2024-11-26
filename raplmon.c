#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <float.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <bfdev.h>

#define RAPL_DIRECTORY "/sys/class/powercap"
#define RAPL_PREFIX "intel-rapl:"
#define RAPL_NLEN 256

#define HOURS_PER_YEAR (365 * 24)
#define FEE_0_5_WH_YEAR (HOURS_PER_YEAR * 0.0005)
#define FEE_1_2_WH_YEAR (HOURS_PER_YEAR * 0.0012)

typedef struct {
    bfdev_list_head_t list;
    double max, min;
    double total, power;
    uint64_t last;
    bool already;

    char path[PATH_MAX + 1];
    char name[RAPL_NLEN + 1];
    char energy[PATH_MAX + 1];
} sensor_t;

static
BFDEV_LIST_HEAD(sensors);

static unsigned int
path_align, name_align;

static volatile bool
signal_exit;

static unsigned int
sample_count;

static long
sensor_sort(const bfdev_list_head_t *n1, const bfdev_list_head_t *n2, void *p)
{
    sensor_t *s1, *s2;

    s1 = bfdev_list_entry(n1, sensor_t, list);
    s2 = bfdev_list_entry(n2, sensor_t, list);

    return strcmp(s1->path, s2->path);
}

static size_t
path_read(const char *path, char *buff, size_t size)
{
    ssize_t length;
    int fd;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("failed to open file");
        exit(errno);
    }

    length = read(fd, buff, size);
    if (length <= 0) {
        perror("failed to read file");
        exit(errno);
    }

    close(fd);
    return length;
}

static void
discovery_sensors(void)
{
    sensor_t *sensor;
    DIR *pcap;

    pcap = opendir(RAPL_DIRECTORY);
    if (!pcap) {
        perror("failed to open powercap directory");
        exit(errno);
    }

    for (;;) {
        char name[PATH_MAX + 1], energy[PATH_MAX + 1];
        struct dirent *dirent;
        size_t length;

        dirent = readdir(pcap);
        if (!dirent)
            break;

        if (strncmp(dirent->d_name, RAPL_PREFIX, sizeof(RAPL_PREFIX) - 1))
            continue;

        snprintf(name, PATH_MAX, RAPL_DIRECTORY "/%s/name", dirent->d_name);
        if (access(name, R_OK))
            continue;

        snprintf(energy, PATH_MAX, RAPL_DIRECTORY "/%s/energy_uj", dirent->d_name);
        if (access(energy, R_OK))
            continue;

        sensor = calloc(1, sizeof(sensor_t));
        if (!sensor) {
            perror("failed to malloc sensor");
            exit(ENOMEM);
        }

        length = path_read(name, sensor->name, RAPL_NLEN);
        sensor->name[length - 1] = '\0';
        strncpy(sensor->path, dirent->d_name, PATH_MAX);
        strncpy(sensor->energy, energy, PATH_MAX);
        sensor->max = DBL_MIN;
        sensor->min = DBL_MAX;
        bfdev_list_add(&sensors, &sensor->list);
    }

    closedir(pcap);
    bfdev_list_sort(&sensors, sensor_sort, NULL);
    bfdev_list_for_each_entry(sensor, &sensors, list) {
        bfdev_max_adj(path_align, strlen(sensor->path));
        bfdev_max_adj(name_align, strlen(sensor->name));
    }

    bfdev_list_for_each_entry(sensor, &sensors, list) {
        bfdev_log_debug(
            "discovery sensor: %-*s => %s\n",
            path_align, sensor->path, sensor->name
        );
    }
}

static void
sample_sensors(void)
{
    char buffer[RAPL_NLEN];
    sensor_t *sensor;
    uint64_t uj;

    bfdev_list_for_each_entry(sensor, &sensors, list) {
        double power;

        path_read(sensor->energy, buffer, RAPL_NLEN);
        uj = strtoll(buffer, NULL, 10);
        power = (double)(uj - sensor->last) / 1000000;
        sensor->last = uj;

        if (!sensor->already) {
            sensor->already = true;
            continue;
        }

        bfdev_max_adj(sensor->max, power);
        bfdev_min_adj(sensor->min, power);
        sensor->power = power;
    }
}

static void
show_sensors(void)
{
    unsigned int align;
    sensor_t *sensor;

    align = 0;
    bfdev_list_for_each_entry(sensor, &sensors, list)
        bfdev_max_adj(align, snprintf(NULL, 0, "%.4lf", sensor->power));

    bfdev_list_for_each_entry(sensor, &sensors, list) {
        sensor->total += sensor->power;
        bfdev_log_info(
            "%-*s => %-*s = Power: %*.4lfw\n",
            path_align, sensor->path,
            name_align, sensor->name,
            align, sensor->power
        );
    }
}

static void
signal_handler(int sig)
{
    bfdev_log_notice("waitting for exit...\n");
    signal_exit = true;
}

int
main(int argc, const char *const argv[])
{
    if (signal(SIGINT, signal_handler)) {
        perror("failed to register signal");
        return -EFAULT;
    }

    discovery_sensors();
    if (bfdev_list_check_empty(&sensors)) {
        bfdev_log_alert("no available sensor found\n");
        return -ENODEV;
    }

    /* initial sample */
    sample_sensors();
    sleep(1);

    for (;;) {
        unsigned int max_align, min_align, avg_align;
        sensor_t *sensor;

        sample_sensors();
        show_sensors();
        sample_count++;

        bfdev_log_info("--------------------------------\n");
        sleep(1);

        if (!signal_exit)
            continue;

        max_align = 0;
        min_align = 0;
        avg_align = 0;

        bfdev_list_for_each_entry(sensor, &sensors, list) {
            bfdev_max_adj(max_align, snprintf(NULL, 0, "%.4lf", sensor->max));
            bfdev_max_adj(min_align, snprintf(NULL, 0, "%.4lf", sensor->min));
            bfdev_max_adj(avg_align, snprintf(NULL, 0, "%.4lf", sensor->total / sample_count));
        }

        bfdev_list_for_each_entry(sensor, &sensors, list) {
            bfdev_log_info(
                "%-*s => %-*s = Max: %*.4lfw, Min: %*.4lfw, Avg: %*.4lfw\n",
                path_align, sensor->path,
                name_align, sensor->name,
                max_align, sensor->max,
                min_align, sensor->min,
                avg_align, sensor->total / sample_count
            );
        }

        break;
    }

    return 0;
}
