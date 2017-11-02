#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include "bme280-i2c.h"
#include "si1132.h"

const char version[] = "v1.6";

static int pressure;
static int temperature;
static int humidity;

#define  O_TEXT 0
#define  O_JSON 1

float SEALEVELPRESSURE_HPA = 1024.25;
static char * progname = NULL;
static int out_format = O_TEXT;
static char * out_filename = NULL;
static FILE * out_file = NULL;
static void usage() {

    fprintf(stderr, "Usage: %s [-d ] [-f] [-p integer] [-k command] [-w integer] \n", progname);
    exit(1);
}

char *arg_param(char *arg) {
    char *p = strchr(arg, ':');
    if (p) {
        return ++p;
    } else {
        return p;
    }
}

void open_outfile() {
    if ((!out_filename) || (!*out_filename) || (strcmp(out_filename, "-") == 0)) { /* Write samples to stdout */
        out_file = stdout;
    } else {
        out_file = fopen(out_filename, "a");
    }
    if (!out_file) {
        fprintf(stderr, "Unable to open out file %d %s", errno, strerror(errno));
    }
}

void close_outfile() {
    fclose(out_file);
    out_file = NULL;
}

void out_text() {
    fprintf(out_file, "\e[H======== si1132 ========\n");
    fprintf(out_file, "UV_index : %.2f\e[K\n", Si1132_readUV() / 100.0);
    fprintf(out_file, "Visible : %.0f Lux\e[K\n", Si1132_readVisible());
    fprintf(out_file, "IR : %.0f Lux\e[K\n", Si1132_readIR());

    bme280_read_pressure_temperature_humidity(
        (u32*)&pressure, &temperature, (u32*)&humidity);
    fprintf(out_file, "======== bme280 ========\n");
    fprintf(out_file, "temperature : %.2lf 'C\e[K\n", (double)temperature / 100.0);
    fprintf(out_file, "humidity : %.2lf %%\e[K\n",	(double)humidity / 1024.0);
    fprintf(out_file, "pressure : %.2lf hPa\e[K\n", (double)pressure / 100.0);
    fprintf(out_file, "altitude : %f m\e[K\n", bme280_readAltitude(pressure,
            SEALEVELPRESSURE_HPA));
    fflush(out_file);
}

void out_json() {
    time_t timer;
    char buffer[26] = {};
    struct tm* tm_info;

    time(&timer);
    tm_info = localtime(&timer);
    strftime(buffer, sizeof(buffer) - 1, "%Y-%m-%d %H:%M:%S", tm_info);
    bme280_read_pressure_temperature_humidity(
        (u32*)&pressure, &temperature, (u32*)&humidity);
    fprintf(out_file,
            "{\"time\": \"%s\", \"brand\": \"ODROID\", \"model\": \"WB2\", \"id\": 0, \"channel\": 1, \"battery\": \"OK\", \
\"temperature_C\": %.2lf, \"humidity\": %.2lf, \"pressure\": %.2lf, \"altitude\": %f, \
\"uv_index\": %.2f, \"visible\": %.0f, \"ir\": %.0f}\n", buffer,
            (double)temperature / 100.0, (double)humidity / 1024.0, (double)pressure / 100.0, bme280_readAltitude(pressure, SEALEVELPRESSURE_HPA),
            Si1132_readUV() / 100.0, Si1132_readVisible(), Si1132_readIR() );
    fflush(out_file);
//{"time" : "2017-10-13 13:28:37", "brand" : "OS", "model" : "THGR122N", "id" : 88, "channel" : 1, "battery" : "OK", "temperature_C" : 14.000, "humidity" : 65}
}

static int do_exit = 0;

static void sighandler(int signum) {
    if (signum == SIGPIPE) {
        signal(SIGPIPE, SIG_IGN);
    } else {
        fprintf(stderr, "Signal caught, exiting!\n");
    }
    do_exit = 1;
}

int main(int argc, char **argv) {
    int flags = 0;
    char *device = "/dev/i2c-1";
    struct sigaction sigact;

    sigact.sa_handler = sighandler;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);
    sigaction(SIGQUIT, &sigact, NULL);
    sigaction(SIGPIPE, &sigact, NULL);


    if ((progname = strrchr(argv[0], '/')) == NULL)
        progname = argv[0];
    else
        ++progname;

    while ((flags = getopt(argc, argv, "F:")) != -1) {
        switch (flags) {

        case 'o': {
            break;
        }

        case 'F': {
            if (strncmp(optarg, "json", 4) == 0) {
                out_format = O_JSON;
                out_filename = arg_param(optarg);
            } else if (strncmp(optarg, "text", 4) == 0) {
                out_format = O_TEXT;
                out_filename = arg_param(optarg);
            } else {
                fprintf(stderr, "Invalid output format %s\n", optarg);
                usage();
            }
            break;
        }
        case 'd': {
            device = strdup(optarg);
            break;
        }
        default: {
            usage();
            break;
        }
        }
    }

    si1132_begin(device);
    bme280_begin(device);

    fprintf(stderr, "WEATHER-BOARD %s\n", version);

    open_outfile();

    while (!do_exit) {

        if ((out_file != stdout) && (access(out_filename, F_OK) == -1)) {
            close_outfile();
            open_outfile();
        }

        if (out_format == O_TEXT) {
            out_text();
        } else {
            out_json();
        }
        usleep(20000000);
    }

    close_outfile();
    return 0;
}
