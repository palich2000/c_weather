#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "bme280-i2c.h"
#include "si1132.h"

#include "dpid.h"
#include "dmem.h"
#include "dlog.h"
#include "dfork.h"
#include "dsignal.h"
#include "version.h"

static int pressure;
static int temperature;
static int humidity;

#define  O_TEXT 0
#define  O_JSON 1

float SEALEVELPRESSURE_HPA = 1024.25;
static int out_format = O_TEXT;
static char * out_filename = NULL;
static FILE * out_file = NULL;

#define HOSTNAME_SIZE 256
#define CDIR "./"

static char * progname = NULL;
static char * hostname = NULL;
static char * pathname = NULL;
static const char * const application = "weather_board";
static int do_exit = 0;

static void usage() {
    fprintf(stderr, "Usage: %s [-d ] [-f] [-p integer] [-k command] [-w integer] \n", progname);
    exit(1);
}

//------------------------------------------------------------------------------------------------------------
//
// Daemon commands callbacks:
//
//------------------------------------------------------------------------------------------------------------
pid_t main_pid;

enum command_int_t {
    CMD_NONE = 0,
    CMD_RECONFIGURE,
    CMD_SHUTDOWN,
    CMD_RESTART,
    CMD_CHECK,
    CMD_NOT_FOUND = -1,
};

typedef int (* daemon_command_callback_t)(void*);

typedef struct daemon_command_t {
    char * command_name;
    daemon_command_callback_t  command_callback;
    int command_int;
} DAEMON_COMMAND_T;

int check_callback(void * UNUSED(param)) {
    return(10);
}


int reconfigure_callback(void * UNUSED(param)) {

    if (daemon_pid_file_kill(SIGUSR1) < 0) {
        daemon_log(LOG_WARNING, "Failed to reconfiguring");
    } else {
        daemon_log(LOG_INFO, "OK");
    }
    return(10);
}

int shutdown_callback(void * UNUSED(param)) {
    int ret;
    daemon_log(LOG_INFO, "Try to shutdown self....");
    if ((ret = daemon_pid_file_kill_wait(SIGINT, 10)) < 0) {
        daemon_log(LOG_WARNING, "Failed to shutdown daemon %d %s", errno, strerror(errno));
        daemon_log(LOG_WARNING, "Try to terminating self....");
        if (daemon_pid_file_kill_wait(SIGKILL, 0) < 0) {
            daemon_log(LOG_WARNING, "Failed to killing daemon %d %s", errno, strerror(errno));
        } else {
            daemon_log(LOG_WARNING, "Daemon terminated");
        }
    } else
        daemon_log(LOG_INFO, "OK");
    return(10);
}

int restart_callback(void * UNUSED(param)) {
    shutdown_callback(NULL);
    return(0);
}

DAEMON_COMMAND_T daemon_commands[] = {
    {command_name: "reconfigure", command_callback: reconfigure_callback, command_int: CMD_RECONFIGURE},
    {command_name: "shutdown", command_callback: shutdown_callback, command_int: CMD_SHUTDOWN},
    {command_name: "restart", command_callback: restart_callback, command_int: CMD_RESTART},
    {command_name: "check", command_callback: check_callback, command_int: CMD_CHECK},
};

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
        daemon_log(LOG_ERR, "Unable to open out file %d %s", errno, strerror(errno));
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

    if (bme280_read_pressure_temperature_humidity(
                (u32*)&pressure, &temperature, (u32*)&humidity) == -1) {
        daemon_log(LOG_ERR, "%s Error communication with bme280", __FUNCTION__);
    }
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
    if (bme280_read_pressure_temperature_humidity(
                (u32*)&pressure, &temperature, (u32*)&humidity) == -1) {
        daemon_log(LOG_ERR, "%s Error communication with bme280", __FUNCTION__);
        return;
    }

    fprintf(out_file,
            "{\"time\": \"%s\", \"brand\": \"ODROID\", \"model\": \"WB2\", \"id\": 0, \"channel\": 1, \"battery\": \"OK\", \
\"temperature_C\": %.2lf, \"humidity\": %.2lf, \"pressure\": %.2lf, \"altitude\": %f, \
\"uv_index\": %.2f, \"visible\": %.0f, \"ir\": %.0f}\n", buffer,
            (double)temperature / 100.0, (double)humidity / 1024.0, (double)pressure / 100.0, bme280_readAltitude(pressure, SEALEVELPRESSURE_HPA),
            Si1132_readUV() / 100.0, Si1132_readVisible(), Si1132_readIR() );
    fflush(out_file);
}


static
void * main_loop (void * p) {
    daemon_log(LOG_INFO, "%s started", __FUNCTION__);
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

        int c_delay = 0;
        while ((!do_exit) && (c_delay < 20)) {
            sleep(1);
            c_delay++;
        }

    }

    daemon_log(LOG_INFO, "%s finished", __FUNCTION__);
    return NULL;
}

//------------------------------------------------------------------------------------------------------------
//
// Start Program:
//
//------------------------------------------------------------------------------------------------------------

int
main (int argc, char *const *argv) {
    int flags;
    int daemonize = true;
    int debug = 0;
    char * command = NULL;
    pid_t pid;
    pthread_t main_th = 0;
    char *device = "/dev/i2c-1";

    int    fd, sel_res;

    daemon_pid_file_ident = daemon_log_ident = application;

    tzset();

    if ((progname = strrchr(argv[0], '/')) == NULL)
        progname = argv[0];
    else
        ++progname;

    if (strrchr(argv[0], '/') == NULL)
        pathname = xstrdup(CDIR);
    else {
        pathname = xmalloc(strlen(argv[0]) + 1);
        strncpy(pathname, argv[0], (strrchr(argv[0], '/') - argv[0]) + 1);
    }

    if (chdir(pathname) < 0) {
        daemon_log(LOG_ERR, "chdir error: %s", strerror(errno));
    }

    FREE(pathname);

    pathname = get_current_dir_name();

    hostname = calloc(1, HOSTNAME_SIZE);
    gethostname(hostname, HOSTNAME_SIZE - 1);

    daemon_log_upto(LOG_INFO);
    daemon_log(LOG_INFO, "%s %s", pathname, progname);

    while ((flags = getopt(argc, argv, "i:fF:D:dk:")) != -1) {
        switch (flags) {

        case 'k': {
            command = xstrdup(optarg);
            break;
        }
        case 'i': {
            daemon_pid_file_ident = daemon_log_ident = xstrdup(optarg);
            break;
        }
        case 'f' : {
            daemonize = false;
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
                daemon_log(LOG_ERR, "Invalid output format %s", optarg);
                usage();
            }
            break;
        }
        case 'd': {
            debug++;
            daemon_log_upto(LOG_DEBUG);
            break;
        }
        case 'D': {
            device = strdup(optarg);
            break;
        }
        default: {
            usage();
            break;
        }
        }
    }

    if (debug) {
        daemon_log(LOG_DEBUG,    "**************************");
        daemon_log(LOG_DEBUG,    "* WARNING !!! Debug mode *");
        daemon_log(LOG_DEBUG,    "**************************");
    }

    daemon_log(LOG_INFO, "%s ver %s [%s %s %s] started", application,  git_version, git_branch, __DATE__, __TIME__);
    daemon_log(LOG_INFO, "***************************************************************************");
    daemon_log(LOG_INFO, "pid file: %s", daemon_pid_file_proc());
    if (command) {
        int r = CMD_NOT_FOUND;
        for (unsigned int i = 0; i < (sizeof(daemon_commands) / sizeof(daemon_commands[0])); i++) {
            if ((strcasecmp(command, daemon_commands[i].command_name) == 0) && (daemon_commands[i].command_callback)) {
                if ((r = daemon_commands[i].command_callback(pathname)) != 0) exit(abs(r - 10));
            }
        }
        if (r == CMD_NOT_FOUND) {
            daemon_log(LOG_ERR, "command \"%s\" not found.", command);
            usage();
        }
    }
    FREE(command);

    /* initialize PRNG */
    srand ((unsigned int) time (NULL));

    if ((pid = daemon_pid_file_is_running()) >= 0) {
        daemon_log(LOG_ERR, "Daemon already running on PID file %u", pid);
        return 1;
    }

    daemon_log(LOG_INFO, "Make a daemon");

    daemon_retval_init();
    if ((daemonize) && ((pid = daemon_fork()) < 0)) {
        return 1;
    } else if ((pid) && (daemonize)) {
        int ret;
        if ((ret = daemon_retval_wait(20)) < 0) {
            daemon_log(LOG_ERR, "Could not recieve return value from daemon process.");
            return 255;
        }
        if (ret == 0) {
            daemon_log(LOG_INFO, "Daemon started.");
        } else {
            daemon_log(LOG_ERR, "Daemon dont started, returned %i as return value.", ret);
        }
        return ret;
    } else {

        if (daemon_pid_file_create() < 0) {
            daemon_log(LOG_ERR, "Could not create PID file (%s).", strerror(errno));
            daemon_retval_send(1);
            goto finish;
        }

        if (daemon_signal_init(/*SIGCHLD,*/SIGINT, SIGTERM, SIGQUIT, SIGHUP, SIGUSR1, SIGUSR2, SIGHUP, /*SIGSEGV,*/ 0) < 0) {
            daemon_log(LOG_ERR, "Could not register signal handlers (%s).", strerror(errno));
            daemon_retval_send(1);
            goto finish;
        }

        daemon_retval_send(0);
        daemon_log(LOG_INFO, "%s ver %s [%s %s %s] started", application,  git_version, git_branch, __DATE__, __TIME__);

        struct rlimit core_lim;

        if (getrlimit(RLIMIT_CORE, &core_lim) < 0) {
            daemon_log(LOG_ERR, "getrlimit RLIMIT_CORE error:%s", strerror(errno));
        } else {
            daemon_log(LOG_INFO, "core limit is cur:%2ld max:%2ld", core_lim.rlim_cur, core_lim.rlim_max );
            core_lim.rlim_cur = -1;
            core_lim.rlim_max = -1;
            if (setrlimit(RLIMIT_CORE, &core_lim) < 0) {
                daemon_log(LOG_ERR, "setrlimit RLIMIT_CORE error:%s", strerror(errno));
            } else {
                daemon_log(LOG_INFO, "core limit set cur:%2ld max:%2ld", core_lim.rlim_cur, core_lim.rlim_max );
            }
        }

        main_pid = syscall(SYS_gettid);

        si1132_begin(device);
        bme280_begin(device);
	umask(0022);
        open_outfile();

        pthread_create( &main_th, NULL, main_loop, NULL);
// main

        fd_set fds;
        FD_ZERO(&fds);
        fd = daemon_signal_fd();
        FD_SET(fd,  &fds);

        while (!do_exit) {
            struct timeval tv;
            tv.tv_sec  = 0;
            tv.tv_usec = 100000;
            fd_set fds2 = fds;
            if ((sel_res = select(FD_SETSIZE, &fds2, 0, 0, &tv)) < 0) {

                if (errno == EINTR)
                    continue;

                daemon_log(LOG_ERR, "select() error:%d %s", errno,  strerror(errno));
                break;
            }
            if (FD_ISSET(fd, &fds2)) {
                int sig;

                if ((sig = daemon_signal_next()) <= 0) {
                    daemon_log(LOG_ERR, "daemon_signal_next() failed.");
                    break;
                }

                switch (sig) {
                case SIGCHLD: {
                    int ret = 0;
                    daemon_log(LOG_INFO, "SIG_CHLD");
                    wait(&ret);
                    daemon_log(LOG_INFO, "RET=%d", ret);
                }
                break;

                case SIGINT:
                case SIGQUIT:
                case SIGTERM:
                    daemon_log(LOG_WARNING, "Got SIGINT, SIGQUIT or SIGTERM");
                    do_exit = true;
                    break;

                case SIGUSR1: {
                    daemon_log(LOG_WARNING, "Got SIGUSR1");
                    daemon_log(LOG_WARNING, "Enter in debug mode, to stop send me USR2 signal");
                    daemon_log_upto(LOG_DEBUG);
                    break;
                }
                case SIGUSR2: {
                    daemon_log(LOG_WARNING, "Got SIGUSR2");
                    daemon_log(LOG_WARNING, "Leave debug mode");
                    daemon_log_upto(LOG_INFO);
                    break;
                }
                case SIGHUP:
                    daemon_log(LOG_WARNING, "Got SIGHUP");
                    break;

                case SIGSEGV:
                    daemon_log(LOG_ERR, "Seg fault. Core dumped to /tmp/core.");
                    if (chdir("/tmp") < 0) {
                        daemon_log(LOG_ERR, "Chdir to /tmp error: %s", strerror(errno));
                    }
                    signal(sig, SIG_DFL);
                    kill(getpid(), sig);
                    break;

                default:
                    daemon_log(LOG_ERR, "UNKNOWN SIGNAL:%s", strsignal(sig));
                    break;

                }
            }
        }

    }

finish:
    daemon_log(LOG_INFO, "Exiting...");
    pthread_join(main_th, NULL);
    close_outfile();
    FREE(hostname);
    FREE(pathname);
    daemon_retval_send(-1);
    daemon_signal_done();
    daemon_pid_file_remove();
    daemon_log(LOG_INFO, "Exit");
    exit(0);
}
//------------------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------------------
