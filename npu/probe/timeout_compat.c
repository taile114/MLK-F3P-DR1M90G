/* Bounded process-group supervisor for the paired image without timeout. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;
static void remember_signal(int number) { interrupted = number; }

static unsigned duration(const char *text, int zero_allowed) {
    const char *p;
    unsigned long result;
    for (p = text; *p; ++p) if (*p < '0' || *p > '9') return UINT_MAX;
    if (p == text) return UINT_MAX;
    errno = 0;
    result = strtoul(text, NULL, 10);
    if (errno || result > 3600 || (!zero_allowed && !result)) return UINT_MAX;
    return (unsigned) result;
}

static double now(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return -1;
    return value.tv_sec + value.tv_nsec / 1e9;
}

static void signal_child(pid_t child, int number) {
    /* The direct child remains unreaped here, so its PID cannot be reused. */
    if (kill(-child, number) < 0 && errno == ESRCH) (void)kill(child, number);
}

int main(int argc, char **argv) {
    unsigned grace = 5, seconds;
    int argument = 1, status, sent = 0, killed = 0, timed_out = 0, forwarded = 0;
    double begin, signal_time = 0;
    pid_t child;
    struct sigaction action;
    const struct timespec interval = {0, 10000000};
    if (argc > 3 && !strcmp(argv[1], "-k")) {
        grace = duration(argv[2], 1);
        argument = 3;
    }
    if (argc <= argument + 1 || grace == UINT_MAX ||
        (seconds = duration(argv[argument], 0)) == UINT_MAX) {
        fprintf(stderr, "Usage: timeout [-k KILL_SECONDS] SECONDS COMMAND [ARGS...]\n");
        return 125;
    }
    ++argument;
    begin = now();
    if (begin < 0) { perror("clock_gettime"); return 125; }
    memset(&action, 0, sizeof action);
    action.sa_handler = remember_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL) ||
        sigaction(SIGHUP, &action, NULL)) { perror("sigaction"); return 125; }
    child = fork();
    if (child < 0) { perror("fork"); return 125; }
    if (child == 0) {
        if (setpgid(0, 0) < 0) { perror("setpgid"); _exit(125); }
        action.sa_handler = SIG_DFL;
        (void)sigaction(SIGINT, &action, NULL);
        (void)sigaction(SIGTERM, &action, NULL);
        (void)sigaction(SIGHUP, &action, NULL);
        execvp(argv[argument], argv + argument);
        status = errno == ENOENT ? 127 : 126;
        perror("execvp");
        _exit(status);
    }
    (void)setpgid(child, child);
    for (;;) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        double current;
        if (result == child) break;
        if (result < 0 && errno != EINTR) {
            perror("waitpid");
            signal_child(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            return 125;
        }
        current = now();
        if (current < 0) {
            signal_child(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            return 125;
        }
        if (!sent && (interrupted || current - begin >= seconds)) {
            forwarded = interrupted;
            timed_out = !forwarded;
            fprintf(stderr, "timeout_compat: %s; signal=%d\n",
                    timed_out ? "deadline_reached" : "interrupted", forwarded ? forwarded : SIGTERM);
            signal_child(child, forwarded ? forwarded : SIGTERM);
            signal_time = current;
            sent = 1;
        }
        if (sent && !killed && current - signal_time >= grace) {
            fprintf(stderr, "timeout_compat: grace_expired; signal=%d\n", SIGKILL);
            signal_child(child, SIGKILL);
            killed = 1;
        }
        (void)nanosleep(&interval, NULL);
    }
    if (forwarded) return 128 + forwarded;
    if (timed_out) return 124;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
}
