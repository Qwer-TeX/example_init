#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <stdbool.h>

#define SHELL "/bin/sh"
#define MAX_PROCESSES 10
#define CONFIG_FILE "/etc/inittab"
#define LOG_FILE "/var/log/init.log"
#define MAX_RUNLEVELS 5
#define HEALTH_CHECK_INTERVAL 5 // Check every 5 seconds

typedef struct {
    pid_t pid;
    char command[256];
    int runlevel;
    bool active;
} Process;

Process processes[MAX_PROCESSES];
int process_count = 0;
int current_runlevel = 0;

void log_message(const char *message) {
    FILE *log_file = fopen(LOG_FILE, "a");
    if (log_file) {
        fprintf(log_file, "%s\n", message);
        fclose(log_file);
    }
}

void handle_signal(int sig) {
    if (sig == SIGCHLD) {
        // Reap all dead children
        while (1) {
            pid_t pid = waitpid(-1, NULL, WNOHANG);
            if (pid <= 0) break;

            for (int i = 0; i < process_count; i++) {
                if (processes[i].pid == pid) {
                    char log_msg[512];
                    snprintf(log_msg, sizeof(log_msg), "Process %s (PID %d) finished", processes[i].command, pid);
                    log_message(log_msg);
                    processes[i].active = false; // Mark as inactive
                    break;
                }
            }
        }
    }
}

void start_process(const char *command, int runlevel) {
    if (process_count >= MAX_PROCESSES) {
        log_message("Max processes reached");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        log_message("Failed to fork process");
        return;
    }

    if (pid == 0) {
        // Child process
        execl(command, command, NULL);
        perror("execl");
        exit(EXIT_FAILURE);
    } else {
        // Parent process
        processes[process_count++] = (Process){pid, "", runlevel, true};
        strncpy(processes[process_count - 1].command, command, sizeof(processes[process_count - 1].command));
        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "Started process: %s with PID: %d for runlevel: %d", command, pid, runlevel);
        log_message(log_msg);
    }
}

void init_processes() {
    FILE *config = fopen(CONFIG_FILE, "r");
    if (!config) {
        perror("Could not open configuration file");
        log_message("Could not open configuration file");
        return;
    }

    char command[256];
    int runlevel;
    while (fgets(command, sizeof(command), config)) {
        // Each line should have "runlevel command"
        if (sscanf(command, "%d %[^\n]", &runlevel, command) == 2) {
            if (runlevel == current_runlevel) {
                start_process(command, runlevel);
            }
        }
    }

    fclose(config);
}

void switch_runlevel(int new_runlevel) {
    if (new_runlevel < 0 || new_runlevel >= MAX_RUNLEVELS) {
        log_message("Invalid runlevel");
        return;
    }

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "Switching from runlevel %d to %d", current_runlevel, new_runlevel);
    log_message(log_msg);

    current_runlevel = new_runlevel;

    // Restart processes according to the new runlevel
    for (int i = 0; i < process_count; i++) {
        if (processes[i].active) {
            kill(processes[i].pid, SIGTERM); // Terminate existing processes
        }
    }
    process_count = 0; // Clear current processes
    init_processes(); // Start new processes for the new runlevel
}

void health_check() {
    while (1) {
        sleep(HEALTH_CHECK_INTERVAL);
        for (int i = 0; i < process_count; i++) {
            if (!processes[i].active) {
                // Process has finished, restart it
                char log_msg[512];
                snprintf(log_msg, sizeof(log_msg), "Restarting process: %s", processes[i].command);
                log_message(log_msg);
                start_process(processes[i].command, processes[i].runlevel);
            }
        }
    }
}

int main(int argc, char *argv[]) {
    // Set up signal handler for SIGCHLD
    signal(SIGCHLD, handle_signal);

    // Initialization message
    log_message("Starting init...");

    // Start services from configuration
    init_processes();

    // Start health check thread
    if (fork() == 0) {
        health_check(); // Child process for health checking
        exit(0);
    }

    // Main loop
    while (1) {
        sleep(10); // Main loop doing nothing, waiting for signals
    }

    return 0;
}
