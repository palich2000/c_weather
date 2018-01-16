#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/syscall.h>   /* For SYS_xxx definitions */
#include <pthread.h>
#include "dlog.h"

enum daemon_log_flags daemon_log_use = DAEMON_LOG_AUTO | DAEMON_LOG_STDERR;
const char* daemon_log_ident = NULL;

unsigned int def_prio = LOG_MASK(LOG_EMERG) | LOG_MASK(LOG_ALERT) | LOG_MASK(LOG_CRIT) | LOG_MASK(LOG_ERR) | LOG_MASK(LOG_WARNING) \
                        |  LOG_MASK(LOG_NOTICE) | LOG_MASK(LOG_INFO) | LOG_MASK(LOG_DEBUG);

char * prio_names[] = {"[emerg]",
                       "[alert]",
                       "[crit ]",
                       "[error]",
                       "[warn ]",
                       "[notice]",
                       "[info] ",
                       "[debug]"
                      };

unsigned int    daemon_get_prio(void) {
    return(def_prio);
}

char * daemon_prio_name(unsigned int priority) {
    if (priority < sizeof(prio_names) / sizeof(prio_names[0])) {
        return(prio_names[priority]);
    } else {
        return("[unk] ");
    }
}

unsigned int daemon_log_upto(unsigned int prio) {
    unsigned int save_prio = def_prio;
    def_prio = LOG_UPTO(prio);
    return save_prio;
}

static unsigned long get_tid(void) {
    static __thread unsigned long _tid = 0;
    if (!_tid) {
        _tid = syscall(SYS_gettid);
    }
    return(_tid);
}

void daemon_logv(int prio, const char* template, va_list arglist) {
    int saved_errno;

    if ((LOG_MASK(prio) & def_prio) == 0 ) return;

    saved_errno = errno;
    va_list arglist1, arglist2, arglist3;
    va_copy(arglist1, arglist);
    va_copy(arglist2, arglist);
    va_copy(arglist3, arglist);
    if (daemon_log_use & DAEMON_LOG_SYSLOG) {
        char buffer[256] = {};
        openlog(daemon_log_ident ? daemon_log_ident : "UNKNOWN", 0, /*LOG_DAEMON*/ LOG_LOCAL1 );
        vsnprintf(buffer, sizeof(buffer), template, arglist1);
        buffer[sizeof(buffer) - 1] = 0;

        char * ps = buffer, * pb = buffer;
        while (*pb) {
            if (*pb == '\n') {
                *pb = 0;
                syslog(prio | LOG_DAEMON, "%s[%05ld]%s", daemon_prio_name(prio), get_tid(), ps);
                ps = pb + 1;
            } else {
                if ((*pb == '\r') || (*pb == '\t'))  *pb = ' ';
            }
            pb++;
        }
        if (pb != ps) {
            syslog(prio | LOG_DAEMON, "%s[%05ld]%s", daemon_prio_name(prio), get_tid(), ps);
        }
    }

    if ((daemon_log_use & DAEMON_LOG_STDERR) || (daemon_log_use & DAEMON_LOG_STDOUT)) {
        time_t curtime = time (NULL);
        struct tm *now = localtime(&curtime);
        struct timeval hp_now;
        char buffer[512] = {};
        char time_buffer[21] = {};

        gettimeofday(&hp_now, NULL);
        strftime (time_buffer, 20, "%T", now);

        int ll = snprintf(buffer, sizeof(buffer) - 1, "%s.%04d %s [%05ld] ", time_buffer, (int)hp_now.tv_usec / 100, daemon_prio_name(prio), get_tid());
        buffer[sizeof(buffer) - 1] = 0;

        int l = sizeof(buffer) - ll - 1;

        strncat(buffer, template, l);
        buffer[sizeof(buffer) - 1] = 0;

        if (daemon_log_use & DAEMON_LOG_STDERR) {
            vfprintf(stderr, buffer, arglist2);
            fprintf(stderr, "\n");
        }

        if (daemon_log_use & DAEMON_LOG_STDOUT) {
            vfprintf(stdout, buffer, arglist3);
            fprintf(stdout, "\n");
        }
    }

    errno = saved_errno;
}

void daemon_log(int prio, const char* template, ...) {
    va_list arglist;

    if ((LOG_MASK(prio) & def_prio) == 0 ) return;

    va_start(arglist, template);
    daemon_logv(prio, template, arglist);
    va_end(arglist);
}

char *daemon_ident_from_argv0(char *argv0) {
    char *p;

    if ((p = strrchr(argv0, '/')))
        return p + 1;

    return argv0;
}

unsigned int  log_check_prio(unsigned int priority) {
    return((LOG_MASK(priority) & def_prio) != 0 );
}


static pthread_mutex_t _indent_mtx = PTHREAD_MUTEX_INITIALIZER;
static __thread int _indent = 0;

static int indent_get() {
    int a;
    pthread_mutex_lock(&_indent_mtx);
    a = _indent;
    pthread_mutex_unlock(&_indent_mtx);
    return(a);
}

void indent_set(int indent) {
    pthread_mutex_lock(&_indent_mtx);
    _indent = indent;
    pthread_mutex_unlock(&_indent_mtx);
}

static bool _daemon_trace_on = true;

void daemon_enter(const char * func_name, const char* template, ...) {
    static pthread_mutex_t tmp_mtx = PTHREAD_MUTEX_INITIALIZER;
    static __thread char tmp[256];

    indent_set(indent_get() + 1);
    if (_daemon_trace_on) {
        pthread_mutex_lock(&tmp_mtx);
        int ll = snprintf(tmp, sizeof(tmp) - 1, "%*c>[%s] ", indent_get(), ' ', func_name);
        tmp[sizeof(tmp) - 1] = 0;
        int l = sizeof(tmp) - ll - 1;
        strncat(tmp, template, l);
        tmp[sizeof(tmp) - 1] = 0;
        va_list arglist;
        va_start(arglist, template);
        daemon_logv(LOG_INFO, tmp, arglist);
        va_end(arglist);
        pthread_mutex_unlock(&tmp_mtx);
    }
}

void daemon_leave(const char * func_name, const char* template, ...) {
    static pthread_mutex_t tmp_mtx = PTHREAD_MUTEX_INITIALIZER;
    static __thread char tmp[256];

    if (_daemon_trace_on) {
        pthread_mutex_lock(&tmp_mtx);
        int ll = snprintf(tmp, sizeof(tmp) - 1, "%*c<[%s] ", indent_get(), ' ', func_name);
        tmp[sizeof(tmp) - 1] = 0;
        int l = sizeof(tmp) - ll - 1;
        strncat(tmp, template, l);
        tmp[sizeof(tmp) - 1] = 0;
        va_list arglist;
        va_start(arglist, template);
        daemon_logv(LOG_INFO, tmp, arglist);
        va_end(arglist);
        pthread_mutex_unlock(&tmp_mtx);
    }
    indent_set(indent_get() - 1);
}

void daemon_trace_switch(bool on) {
    _daemon_trace_on = on;
}

bool daemon_trace_switch_get() {
    return(_daemon_trace_on);
}

void daemon_trace_indent_reset_after_error() {
    indent_set(0);
}

void daemon_trace(const char * func_name, const char* template, ...) {
    static pthread_mutex_t tmp_mtx = PTHREAD_MUTEX_INITIALIZER;
    static __thread char tmp[256];
    if (_daemon_trace_on) {
        pthread_mutex_lock(&tmp_mtx);
        int ll = snprintf(tmp, sizeof(tmp) - 1, "%*c*[%s] ", indent_get(), ' ', func_name);
        tmp[sizeof(tmp) - 1] = 0;
        int l = sizeof(tmp) - ll - 1;
        strncat(tmp, template, l);
        tmp[sizeof(tmp) - 1] = 0;
        va_list arglist;
        va_start(arglist, template);
        daemon_logv(LOG_INFO, tmp, arglist);
        va_end(arglist);
        pthread_mutex_unlock(&tmp_mtx);
    }
}
