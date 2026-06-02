#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>
#include <syslog.h>
#include <time.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdarg.h>
#include <getopt.h>
#include <libgen.h>

static volatile sig_atomic_t received_signal = 0;
static volatile pid_t child_pid = 0;
static bool use_syslog = false;

static void write_timestamp(char *out, size_t out_size)
{
    time_t now = time(NULL);
    struct tm *local_time = localtime(&now);
    strftime(out, out_size, "[%H:%M:%S]", local_time);
}

static void log_info(const char *format, ...)
{
    char timestamp[16];
    write_timestamp(timestamp, sizeof(timestamp));
    fprintf(stdout, "%s ", timestamp);
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
    if (use_syslog) {
        va_start(args, format);
        vsyslog(LOG_INFO, format, args);
        va_end(args);
    }
    fflush(stdout);
}

static void log_error(const char *format, ...)
{
    char timestamp[16];
    write_timestamp(timestamp, sizeof(timestamp));
    fprintf(stderr, "%s ", timestamp);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    if (use_syslog) {
        va_start(args, format);
        vsyslog(LOG_ERR, format, args);
        va_end(args);
    }
    fflush(stderr);
}

static void signal_handler(int signum)
{
    received_signal = signum;
    if (child_pid > 0) {
        kill(child_pid, signum);
    }
}

// If the child already prefixed the line with our [HH:MM:SS] stamp, return the text after it (so we
// do not double-stamp); otherwise return the line unchanged so the caller knows to add a stamp.
static const char *body_after_timestamp(const char *line)
{
    int hour, minute, second;
    int consumed;

    if (sscanf(line, "[%d:%d:%d]%n", &hour, &minute, &second, &consumed) != 3) {
        return line;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return line;
    }

    const char *body = line + consumed;
    while (*body == ' ' || *body == '\t') {
        body++;
    }
    return body;
}

static void log_line(const char *line, const char *suffix)
{
    const char *body = body_after_timestamp(line);

    if (body == line) {
        char timestamp[16];
        write_timestamp(timestamp, sizeof(timestamp));
        fprintf(stderr, "%s %s%s", timestamp, line, suffix);
    } else {
        fprintf(stderr, "%s%s", line, suffix);
    }

    if (use_syslog) {
        syslog(LOG_INFO, "%s", body);
    }
}

static void log_child_output(int pipe_fd)
{
    char chunk[4096];
    ssize_t bytes_read;

    while ((bytes_read = read(pipe_fd, chunk, sizeof(chunk) - 1)) > 0) {
        chunk[bytes_read] = '\0';
        char *segment = chunk;
        char *newline_at;

        while ((newline_at = strchr(segment, '\n')) != NULL) {
            *newline_at = '\0';
            log_line(segment, "\n");
            segment = newline_at + 1;
        }

        if (*segment != '\0') {
            log_line(segment, "");
        }
    }
}

static void print_usage(const char *program_name)
{
    printf("Usage: %s [options] <command> [args...]\n", program_name);
    printf("\n");
    printf("Options:\n");
    printf("  --retry <seconds>   Retry delay in seconds (default: 3)\n");
    printf("  --syslog            Enable syslog logging\n");
    printf("  --help              Show this help\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s sleep 5\n", program_name);
    printf("  %s --retry 5 --syslog /path/to/program arg1 arg2\n", program_name);
}

int main(int argc, char *argv[])
{
    int retry_seconds = 3;
    int retry_count = 0;
    int status;
    int exit_code = 0;
    char **child_argv = NULL;
    int child_argc = 0;
    struct sigaction signal_action;
    int option;

    enum {
        OPT_RETRY = 1,
        OPT_SYSLOG,
        OPT_HELP,
    };

    static struct option long_options[] = {
        {"retry", required_argument, 0, OPT_RETRY},
        {"syslog", no_argument, 0, OPT_SYSLOG},
        {"help", no_argument, 0, OPT_HELP},
        {0, 0, 0, 0}
    };

    while ((option = getopt_long(argc, argv, "+", long_options, NULL)) != -1) {
        switch (option) {
        case OPT_RETRY:
            retry_seconds = atoi(optarg);
            break;
        case OPT_SYSLOG:
            use_syslog = true;
            break;
        case OPT_HELP:
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    child_argc = argc - optind;
    child_argv = &argv[optind];

    if (child_argc == 0) {
        fprintf(stderr, "Error: No command specified\n");
        print_usage(argv[0]);
        return 1;
    }

    if (use_syslog) {
        char *app_name = basename(child_argv[0]);
        openlog(strdup(app_name), LOG_PID, LOG_USER);
        setlogmask(LOG_UPTO(LOG_DEBUG));
    }

    log_info("fake-service - built %s (%s)\n", __DATE__, __FILE__);

    fprintf(stdout, "Command:");
    for (int arg_index = 0; arg_index < child_argc; arg_index++) {
        fprintf(stdout, " %s", child_argv[arg_index]);
    }
    fprintf(stdout, "\n");
    fprintf(stdout, "Retry delay: %d seconds\n", retry_seconds);

    signal_action.sa_handler = signal_handler;
    sigemptyset(&signal_action.sa_mask);
    signal_action.sa_flags = 0;
    sigaction(SIGTERM, &signal_action, NULL);
    sigaction(SIGINT, &signal_action, NULL);
    sigaction(SIGHUP, &signal_action, NULL);
    sigaction(SIGQUIT, &signal_action, NULL);

    while (received_signal == 0) {
        int pipe_fd[2];

        if (pipe(pipe_fd) < 0) {
            log_error("pipe: %s\n", strerror(errno));
            exit(1);
        }

        child_pid = fork();

        if (child_pid < 0) {
            log_error("fork: %s\n", strerror(errno));
            exit(1);
        }

        if (child_pid == 0) {
            close(pipe_fd[0]);
            dup2(pipe_fd[1], STDOUT_FILENO);
            dup2(pipe_fd[1], STDERR_FILENO);
            close(pipe_fd[1]);

            signal(SIGTERM, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            signal(SIGHUP, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);

            execvp(child_argv[0], child_argv);
            fprintf(stderr, "execvp: %s: %s\n", child_argv[0], strerror(errno));
            exit(127);
        }

        close(pipe_fd[1]);

        log_error("Starting child process %d\n", child_pid);

        log_child_output(pipe_fd[0]);
        close(pipe_fd[0]);

        while (1) {
            pid_t waited = waitpid(child_pid, &status, 0);

            if (waited == -1) {
                if (errno == EINTR) {
                    continue;
                }
                log_error("waitpid: %s\n", strerror(errno));
                exit(1);
            }
            break;
        }

        child_pid = 0;

        if (WIFEXITED(status)) {
            exit_code = WEXITSTATUS(status);
            if (exit_code == 0) {
                log_error("Child exited normally with code 0, exiting\n");
                break;
            }
            log_error("Child exited with code %d\n", exit_code);
        } else if (WIFSIGNALED(status)) {
            int term_signal = WTERMSIG(status);
            log_error("Child terminated by signal %d\n", term_signal);
            exit_code = 128 + term_signal;
        }

        if (received_signal != 0) {
            log_error("Monitor received signal %d, exiting\n", received_signal);
            break;
        }

        log_error("Restarting child process in %d seconds (retry %d)\n",
                retry_seconds, ++retry_count);
        sleep(retry_seconds);
    }

    if (use_syslog) {
        closelog();
    }
    return exit_code;
}
