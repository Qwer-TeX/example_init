#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>

#define SHELL "/bin/sh"
#define MAX_PROCESSES 10
#define CONFIG_FILE "/etc/inittab"
#define LOG_FILE "/var/log/init.log"
#define MAX_RUNLEVELS 5
#define HEALTH_CHECK_INTERVAL 5 // Check every 5 seconds
#define MAX_LOG_SIZE (1024 * 1024) // 1 MB

typedef struct {
    pid_t pid;
    char command[256];
    int runlevel;
    bool active;
    char dependencies[256];
    int health_pipe[2]; // Pipe for health reporting
} Process;

Process processes[MAX_PROCESSES];
int process_count = 0;
int current_runlevel = 0;

void log_message(const char *level, const char *message) {
    // Rotate logs if necessary
    struct stat st;
    if (stat(LOG_FILE, &st) == 0 && st.st_size >= MAX_LOG_SIZE) {
        char new_log_file[256];
        snprintf(new_log_file, sizeof(new_log_file), "%s.%ld", LOG_FILE, time(NULL)); // Append timestamp
        rename(LOG_FILE, new_log_file); // Rotate the log file
    }

    FILE *log_file = fopen(LOG_FILE, "a");
    if (log_file) {
        fprintf(log_file, "[%s] %s\n", level, message);
        fclose(log_file);
    }
}

void handle_signal(int sig) {
    if (sig == SIGCHLD) {
        while (1) {
            pid_t pid = waitpid(-1, NULL, WNOHANG);
            if (pid <= 0) break;

            for (int i = 0; i < process_count; i++) {
                if (processes[i].pid == pid) {
                    char log_msg[512];
                    snprintf(log_msg, sizeof(log_msg), "Process %s (PID %d) finished", processes[i].command, pid);
                    log_message("INFO", log_msg);
                    processes[i].active = false; // Mark as inactive
                    break;
                }
            }
        }
    }
}

bool check_dependencies(const char *dependencies) {
    char *dep = strtok(strdup(dependencies), ",");
    while (dep) {
        // Check if each dependency is active
        bool found = false;
        for (int i = 0; i < process_count; i++) {
            if (strcmp(processes[i].command, dep) == 0 && processes[i].active) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false; // If any dependency is not active, return false
        }
        dep = strtok(NULL, ",");
    }
    return true; // All dependencies are satisfied
}

void start_process(const char *command, int runlevel, const char *dependencies) {
    if (process_count >= MAX_PROCESSES) {
        log_message("ERROR", "Max processes reached");
        return;
    }

    if (pipe(processes[process_count].health_pipe) == -1) {
        perror("pipe");
        return;
    }

    if (!check_dependencies(dependencies)) {
        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "Cannot start %s: dependencies not satisfied", command);
        log_message("WARNING", log_msg);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        log_message("ERROR", "Failed to fork process");
        return;
    }

    if (pid == 0) {
        // Child process
        close(processes[process_count].health_pipe[0]); // Close read end
        // Here, you would run the service and write health status to the pipe
        // For example:
        // while (1) {
        //     write(processes[process_count].health_pipe[1], "Healthy", strlen("Healthy")+1);
        //     sleep(HEALTH_CHECK_INTERVAL);
        // }
        execl(command, command, NULL);
        perror("execl");
        exit(EXIT_FAILURE);
    } else {
        // Parent process
        processes[process_count++] = (Process){pid, "", runlevel, true};
        strncpy(processes[process_count - 1].command, command, sizeof(processes[process_count - 1].command));
        strncpy(processes[process_count - 1].dependencies, dependencies, sizeof(processes[process_count - 1].dependencies));
        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "Started process: %s with PID: %d for runlevel: %d", command, pid, runlevel);
        log_message("INFO", log_msg);
    }
}

void init_processes() {
    FILE *config = fopen(CONFIG_FILE, "r");
    if (!config) {
        perror("Could not open configuration file");
        log_message("ERROR", "Could not open configuration file");
        return;
    }

    char command[256];
    int runlevel;
    char dependencies[256];
    while (fgets(command, sizeof(command), config)) {
        // Updated line to include "runlevel command dependencies"
        if (sscanf(command, "%d %[^\n] %[^\n]", &runlevel, command, dependencies) == 3) {
            if (runlevel == current_runlevel) {
                start_process(command, runlevel, dependencies);
            }
        }
    }

    fclose(config);
}

void switch_runlevel(int new_runlevel) {
    if (new_runlevel < 0 || new_runlevel >= MAX_RUNLEVELS) {
        log_message("ERROR", "Invalid runlevel");
        return;
    }

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "Switching from runlevel %d to %d", current_runlevel, new_runlevel);
    log_message("INFO", log_msg);

    current_runlevel = new_runlevel;

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
                log_message("INFO", log_msg);
                start_process(processes[i].command, processes[i].runlevel, processes[i].dependencies);
            } else {
                // Check health status
                char health_status[256];
                if (read(processes[i].health_pipe[0], health_status, sizeof(health_status)) > 0) {
                    log_message("INFO", "Health status received: %s", health_status);
                }
            }
        }
    }
}

void reload_configuration() {
    log_message("INFO", "Reloading configuration...");
    process_count = 0; // Clear current processes
    init_processes(); // Reinitialize processes
}

void graceful_shutdown() {
    log_message("INFO", "Shutting down init system...");
    for (int i = 0; i < process_count; i++) {
        if (processes[i].active) {
            kill(processes[i].pid, SIGTERM); // Send terminate signal
            waitpid(processes[i].pid, NULL, 0); // Wait for the process to finish
            processes[i].active = false; // Mark as inactive
        }
    }
    log_message("INFO", "All processes terminated. Exiting init.");
    exit(0);
}

void manage_services(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s {start|stop|status} <service_name>\n", argv[0]);
        return;
    }

    const char *command = argv[1];
    const char *service_name = argv[2];

    if (strcmp(command, "start") == 0) {
        // Logic to start a service by name
        // Find and start the service process
    } else if (strcmp(command, "stop") == 0) {
        // Logic to stop a service by name
        // Find the service process and send SIGTERM
    } else if (strcmp(command, "status") == 0) {
        // Logic to check the status of a service by name
        // Print whether the service is active or not
    } else {
        printf("Unknown command: %s\n", command);
    }
}

int main(int argc, char *argv[]) {
    signal(SIGTERM, graceful_shutdown);
    signal(SIGCHLD, handle_signal);
    signal(SIGHUP, reload_configuration);

    log_message("INFO", "Starting init...");

    init_processes();

    // Start health check thread
    if (fork() == 0) {
        health_check(); // Child process for health checking
        exit(0);
    }

    // Command-line options for runtime behavior
    if (argc > 1) {
        if (strcmp(argv[1], "switch") == 0 && argc == 3) {
            int new_runlevel = atoi(argv[2]);
            switch_runlevel(new_runlevel);
        } else if (strcmp(argv[1], "manage") == 0) {
            manage_services(argc - 1, argv + 1); // Pass the command-line arguments for managing services
        }
    }

    // Main loop
    while (1) {
        sleep(10); // Main loop doing nothing, waiting for signals
    }

    return 0;
}
