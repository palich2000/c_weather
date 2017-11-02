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
#include <mosquitto.h>

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

static int mosq_error;

void publish_double(struct mosquitto *mosq, char * topic, double value) {
    if (!mosq) return;
    if (mosq_error) return;

    char text[20];
    sprintf(text, "%.2f", value);
    if ((mosq_error = mosquitto_publish (mosq, NULL, topic, strlen (text), text, 0, false)) != 0) {
        if (mosq_error) {
            fprintf (stderr, "Can't publish to Mosquitto server %s\n", mosquitto_strerror(mosq_error));
        }
    }
}


void read_sensors(struct mosquitto *mosq) {
    if (!mosq) return;
    //fprintf(stderr, "%s\n", __FUNCTION__);
    int i_pressure, i_temperature, i_humidity;
    bme280_read_pressure_temperature_humidity((u32*)&i_pressure, &i_temperature, (u32*)&i_humidity);
    double temperature = (double)i_temperature / 100.0;
    double humidity = (double)i_humidity / 1024.0;
    double pressure = (double)i_pressure / 100.0;

    //bme280_readAltitude(i_pressure, SEALEVELPRESSURE_HPA)

    double uv_index = Si1132_readUV() / 100.0;
    double visible = Si1132_readVisible();
    double ir = Si1132_readIR();
    publish_double(mosq, "home/hall/weather/temperature", temperature);
    publish_double(mosq, "home/hall/weather/humidity", humidity);
    publish_double(mosq, "home/hall/weather/pressure", pressure);
    publish_double(mosq, "home/hall/weather/uv_index", uv_index);
    publish_double(mosq, "home/hall/weather/visible", visible);
    publish_double(mosq, "home/hall/weather/ir", ir);
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

void mosq_log_callback(struct mosquitto *mosq, void *userdata, int level, const char *str) {
    switch(level) {
//    case MOSQ_LOG_DEBUG:
//    case MOSQ_LOG_INFO:
//    case MOSQ_LOG_NOTICE:
    case MOSQ_LOG_WARNING:
    case MOSQ_LOG_ERR: {
        fprintf(stderr, "%i:%s\n", level, str);
    }
    }
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

    mosquitto_lib_init();

    struct mosquitto *mosq = NULL;
    const char * mqtt_host = "localhost";
    const char * mqtt_username = "owntracks";
    const char * mqtt_password = "zhopa";
    int mqtt_port = 8883;
    int mqtt_keepalive = 60;
    bool clean_session = true;

    mosq = mosquitto_new(progname, clean_session, NULL);
    if(!mosq) {
        fprintf(stderr, "mosq Error: Out of memory.\n");
    } else {
        mosquitto_log_callback_set(mosq, mosq_log_callback);
        mosquitto_username_pw_set (mosq, mqtt_username, mqtt_password);

	fprintf (stderr, "Try connect to Mosquitto server \n");
        mosq_error = mosquitto_connect (mosq, mqtt_host, mqtt_port, mqtt_keepalive);
        if (mosq_error) {
            fprintf (stderr, "Can't connect to Mosquitto server %s\n", mosquitto_strerror(mosq_error));
        }
    }
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
        read_sensors(mosq);
        usleep(20000000);
        if (mosq_error) {
            mosquitto_disconnect(mosq);
	    fprintf (stderr, "Try connect to Mosquitto server \n");
            mosq_error = mosquitto_connect (mosq, mqtt_host, mqtt_port, mqtt_keepalive);
            if (mosq_error) {
                fprintf (stderr, "Can't connect to Mosquitto server %s\n", mosquitto_strerror(mosq_error));
            }
        }
    }

    close_outfile();

    if (mosq) {
        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);
    }
    mosquitto_lib_cleanup();

    return 0;
}