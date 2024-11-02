#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <errno.h>

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
    char state[32]; // New state attribute
    int health_pipe[2];
    int memory_limit; // Memory limit in bytes
    int cpu_limit;    // CPU limit percentage
} Process;

Process processes[MAX_PROCESSES];
int process_count = 0;
int current_runlevel = 0;

void log_message(const char *level, const char *message) {
    struct stat st;
    if (stat(LOG_FILE, &st) == 0 && st.st_size >= MAX_LOG_SIZE) {
        char new_log_file[256];
        snprintf(new_log_file, sizeof(new_log_file), "%s.%ld", LOG_FILE, time(NULL));
        rename(LOG_FILE, new_log_file);
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
                    processes[i].active = false;
                    strncpy(processes[i].state, "stopped", sizeof(processes[i].state));
                    break;
                }
            }
        }
    }
}

bool check_all_dependencies_active(const char *dependencies) {
    char *dep = strtok(strdup(dependencies), ",");
    while (dep) {
        bool found = false;
        for (int i = 0; i < process_count; i++) {
            if (strcmp(processes[i].command, dep) == 0 && strcmp(processes[i].state, "running") == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
        dep = strtok(NULL, ",");
    }
    return true;
}

void set_resource_limits(pid_t pid, int memory_limit, int cpu_limit) {
    // Set memory limit
    char path[256];
    snprintf(path, sizeof(path), "/sys/fs/cgroup/memory/my_cgroup/memory.limit_in_bytes");
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d", memory_limit);
        fclose(f);
    }

    // Set CPU limit
    snprintf(path, sizeof(path), "/sys/fs/cgroup/cpu/my_cgroup/cpu.cfs_quota_us");
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d", cpu_limit * 10000); // Convert to microseconds
        fclose(f);
    }

    // Add the PID to the cgroup
    snprintf(path, sizeof(path), "/sys/fs/cgroup/memory/my_cgroup/cgroup.procs");
    f = fopen(path, "a");
    if (f) {
        fprintf(f, "%d", pid);
        fclose(f);
    }
}

void start_process(const char *command, int runlevel, const char *dependencies, int memory_limit, int cpu_limit) {
    if (process_count >= MAX_PROCESSES) {
        log_message("ERROR", "Max processes reached");
        return;
    }

    if (!check_all_dependencies_active(dependencies)) {
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
        set_resource_limits(getpid(), memory_limit, cpu_limit); // Set resource limits
        execl(command, command, NULL);
        perror("execl");
        exit(EXIT_FAILURE);
    } else {
        // Parent process
        processes[process_count++] = (Process){pid, "", runlevel, true, "", "running", {0, 0}, memory_limit, cpu_limit};
        strncpy(processes[process_count - 1].command, command, sizeof(processes[process_count - 1].command));
        strncpy(processes[process_count - 1].dependencies, dependencies, sizeof(processes[process_count - 1].dependencies));
        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "Started process: %s with PID: %d for runlevel: %d", command, pid, runlevel);
        log_message("INFO", log_msg);
    }
}

bool start_process_with_retry(const char *command, int runlevel, const char *dependencies, int memory_limit, int cpu_limit, int max_retries) {
    for (int attempt = 0; attempt < max_retries; attempt++) {
        if (check_all_dependencies_active(dependencies)) {
            start_process(command, runlevel, dependencies, memory_limit, cpu_limit);
            return true; // Process started successfully
        }
        sleep(1); // Wait before retrying
    }
    log_message("ERROR", "Failed to start process after retries");
    return false;
}

void init_processes() {
    FILE *config = fopen(CONFIG_FILE, "r");
    if (!config) {
        perror("Could not open configuration file");
        log_message("ERROR", "Could not open configuration file");
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), config)) {
        int runlevel, memory_limit, cpu_limit;
        char command[256], dependencies[256];
        if (sscanf(line, "%d %[^\n] %[^\n] %d %d", &runlevel, command, dependencies, &memory_limit, &cpu_limit) == 5) {
            if (runlevel == current_runlevel) {
                start_process_with_retry(command, runlevel, dependencies, memory_limit, cpu_limit, 3);
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
            kill(processes[i].pid, SIGTERM);
            waitpid(processes[i].pid, NULL, 0);
            processes[i].active = false;
            strncpy(processes[i].state, "stopped", sizeof(processes[i].state));
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
                char log_msg[512];
                snprintf(log_msg, sizeof(log_msg), "Restarting process: %s", processes[i].command);
                log_message("INFO", log_msg);
                start_process_with_retry(processes[i].command, processes[i].runlevel, processes[i].dependencies, processes[i].memory_limit, processes[i].cpu_limit, 3);
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
            kill(processes[i].pid, SIGTERM);
            waitpid(processes[i].pid, NULL, 0);
            processes[i].active = false;
            strncpy(processes[i].state, "stopped", sizeof(processes[i].state));
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
        for (int i = 0; i < process_count; i++) {
            if (strcmp(processes[i].command, service_name) == 0 && !processes[i].active) {
                start_process(processes[i].command, processes[i].runlevel, processes[i].dependencies, processes[i].memory_limit, processes[i].cpu_limit);
                return;
            }
        }
    } else if (strcmp(command, "stop") == 0) {
        // Logic to stop a service by name
        for (int i = 0; i < process_count; i++) {
            if (strcmp(processes[i].command, service_name) == 0 && processes[i].active) {
                kill(processes[i].pid, SIGTERM);
                processes[i].active = false;
                strncpy(processes[i].state, "stopped", sizeof(processes[i].state));
                return;
            }
        }
    } else if (strcmp(command, "status") == 0) {
        // Logic to check the status of a service by name
        for (int i = 0; i < process_count; i++) {
            if (strcmp(processes[i].command, service_name) == 0) {
                printf("Service %s is %s\n", service_name, processes[i].active ? "running" : "stopped");
                return;
            }
        }
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
            manage_services(argc - 1, argv + 1);
        }
    }

    // Main loop
    while (1) {
        sleep(10); // Main loop doing nothing, waiting for signals
    }

    return 0;
}
