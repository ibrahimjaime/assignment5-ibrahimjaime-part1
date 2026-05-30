#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>

#define BUFFER_SIZE 1024
#define FILE_PATH "/var/tmp/aesdsocketdata"

volatile sig_atomic_t keep_running = 1;
int server_fd = -1;

void signal_handler(int signal_number) {
    if (signal_number == SIGINT || signal_number == SIGTERM) {
        syslog(LOG_INFO, "Caught signal, exiting");
        keep_running = 0;
        if (server_fd != -1) {
            close(server_fd);
            server_fd = -1;
        }
    }
}

int append_to_file(const char *data, size_t length) {
    FILE *file = fopen(FILE_PATH, "a");
    if (file == NULL) {
        perror("Error opening or creating file");
        syslog(LOG_ERR, "Failed to open file: %s", FILE_PATH);
        return -1;
    }
    size_t written = fwrite(data, 1, length, file);
    fclose(file);
    return (written == length) ? 0 : -1;
}

void handle_client_data(int client_fd) {
    char read_buf[BUFFER_SIZE];
    char *packet_buf = NULL;
    size_t packet_len = 0;

    while (keep_running) {
        ssize_t bytes_received = recv(client_fd, read_buf, BUFFER_SIZE, 0);
        if (bytes_received > 0) {
            char *new_ptr = realloc(packet_buf, packet_len + bytes_received);
            if (new_ptr == NULL) {
                perror("Memory allocation error (realloc). Packet discarded.");
                free(packet_buf);
                return;
            }
            packet_buf = new_ptr;
            memcpy(packet_buf + packet_len, read_buf, bytes_received);
            packet_len += bytes_received;

            size_t start_idx = 0;
            for (size_t i = 0; i < packet_len; i++) {
                if (packet_buf[i] == '\n') {
                    size_t current_packet_size = (i - start_idx) + 1;
                    append_to_file(packet_buf + start_idx, current_packet_size);

                    FILE *file = fopen(FILE_PATH, "r");
                    if (file != NULL) {
                        char send_buf[BUFFER_SIZE];
                        size_t bytes_read;
                        while ((bytes_read = fread(send_buf, 1, BUFFER_SIZE, file)) > 0) {
                            send(client_fd, send_buf, bytes_read, 0);
                        }
                        fclose(file);
                    }
                    start_idx = i + 1;
                }
            }

            if (start_idx > 0) {
                size_t remaining_len = packet_len - start_idx;
                if (remaining_len > 0) {
                    memmove(packet_buf, packet_buf + start_idx, remaining_len);
                    packet_len = remaining_len;
                    char *shrunk_ptr = realloc(packet_buf, packet_len);
                    if (shrunk_ptr != NULL) packet_buf = shrunk_ptr;
                } else {
                    free(packet_buf); packet_buf = NULL; packet_len = 0;
                }
            }
        } else {
            break; 
        }
    }
    if (packet_buf != NULL) free(packet_buf);
}

int main(int argc, char *argv[]) {
    int daemon_mode = 0;

    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = 1;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
        perror("Error setting up signal handlers");
        closelog();
        return -1;
    }

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation error");
        closelog();
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(9000),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Socket bind error");
        close(server_fd);
        closelog();
        return -1;
    }

    if (daemon_mode) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("Fork failure");
            close(server_fd);
            closelog();
            return -1;
        }

        if (pid > 0) {
            close(server_fd);
            closelog();
            return 0; 
        }

        if (setsid() < 0) {
            perror("Setsid failure");
            close(server_fd);
            closelog();
            return -1;
        }

        if (chdir("/") < 0) {
            perror("Chdir failure");
            close(server_fd);
            closelog();
            return -1;
        }

        int dev_null = open("/dev/null", O_RDWR);
        if (dev_null >= 0) {
            dup2(dev_null, STDIN_FILENO);
            dup2(dev_null, STDOUT_FILENO);
            dup2(dev_null, STDERR_FILENO);
            close(dev_null);
        }
    }

    if (listen(server_fd, 10) < 0) {
        perror("Socket listen error");
        close(server_fd);
        closelog();
        return -1;
    }

    printf("Server listening on port 9000. Press Ctrl+C to exit gracefully...\n");

    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    while (keep_running) {
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_fd < 0) {
            if (!keep_running) break; 
            continue;
        }

        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ipstr, sizeof(ipstr));
        syslog(LOG_INFO, "Accepted connection from %s", ipstr);

        handle_client_data(client_fd);

        close(client_fd);
        syslog(LOG_INFO, "Closed connection from %s", ipstr);
    }

    if (server_fd != -1) {
        close(server_fd);
    }

    unlink(FILE_PATH);
    closelog();
    return 0;
}