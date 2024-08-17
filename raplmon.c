#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <bfdev.h>

#define RAPL_DIRECTORY "/sys/class/powercap"
#define RAPL_PREFIX "intel-rapl:"
#define RAPL_NLEN 256

typedef struct {
    bfdev_list_head_t list;
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

static long
sensor_sort(const bfdev_list_head_t *n1, const bfdev_list_head_t *n2, void *p)
{
    sensor_t *s1, *s2;

    s1 = bfdev_list_entry(n1, sensor_t, list);
    s2 = bfdev_list_entry(n2, sensor_t, list);

    return strcmp(s1->path, s2->path);
}

static void
path_read(const char *path, char *buff, size_t size)
{
    int fd, retval;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("failed to open file");
        exit(errno);
    }

    retval = read(fd, buff, size);
    if (retval < 0) {
        perror("failed to read file");
        exit(errno);
    }

    close(fd);
}

static void
discovery_sensors(void)
{
    char name[PATH_MAX + 1], energy[PATH_MAX + 1];
    sensor_t *sensor;
    struct dirent *dirent;
    DIR *pcap;

    pcap = opendir(RAPL_DIRECTORY);
    if (!pcap) {
        perror("failed to open powercap directory");
        exit(errno);
    }

    for (;;) {
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

        path_read(name, sensor->name, RAPL_NLEN);
        sensor->name[strlen(sensor->name) - 1] = '\0';

        strncpy(sensor->path, dirent->d_name, PATH_MAX);
        strncpy(sensor->energy, energy, PATH_MAX);
        bfdev_list_add(&sensors, &sensor->list);
    }

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
show_sensors(void)
{
    char buffer[RAPL_NLEN];
    sensor_t *sensor;
    uint64_t uj;

    bfdev_list_for_each_entry(sensor, &sensors, list) {
        double power;
        ssize_t retval;
        int fd;

        fd = open(sensor->energy, O_RDONLY);
        if (fd < 0) {
            perror("failed to open energy file");
            exit(errno);
        }

        retval = read(fd, buffer, RAPL_NLEN);
        if (retval <= 0) {
            perror("failed to read energy file");
            exit(errno);
        }

        close(fd);
        buffer[retval - 1] = '\0';
        uj = strtoll(buffer, NULL, 10);
        power = (double)(uj - sensor->last) / 1000000;
        sensor->last = uj;

        if (!sensor->already) {
            sensor->already = true;
            continue;
        }

        bfdev_log_info(
            "%-*s => %-*s = %.4lfw\n",
            path_align, sensor->path,
            name_align, sensor->name,
            power
        );
    }
}

int
main(int argc, const char *const argv[])
{
    discovery_sensors();
    if (bfdev_list_check_empty(&sensors)) {
        bfdev_log_alert("no available sensor found\n");
        return ENODEV;
    }

    for (;;) {
        show_sensors();
        bfdev_log_info("--------------------------------\n");
        sleep(1);
    }

    return 0;
}
