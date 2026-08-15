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
#include <pthread.h>
#include <sys/queue.h>
#include <time.h> 

#include <sys/queue.h>
#include <time.h>

#include <sys/ioctl.h>
#include <errno.h>
#include <stdbool.h>
#include "aesd_ioctl.h"

#ifndef SLIST_FOREACH_SAFE
#define SLIST_FOREACH_SAFE(var, head, field, tvar) \
    for ((var) = SLIST_FIRST((head));              \
         (var) && ((tvar) = SLIST_NEXT((var), field), 1); \
         (var) = (tvar))
#endif

#define BUFFER_SIZE 1024

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#if USE_AESD_CHAR_DEVICE
#define FILE_PATH "/dev/aesdchar"
#else
#define FILE_PATH "/var/tmp/aesdsocketdata"
#endif

volatile sig_atomic_t keep_running = 1;
int server_fd = -1;

pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct thread_data {
    pthread_t thread_id;
    int client_fd;
    struct sockaddr_in client_addr;
    int is_complete;
    SLIST_ENTRY(thread_data) entries;
} thread_data_t;

SLIST_HEAD(slisthead, thread_data) thread_list_head = SLIST_HEAD_INITIALIZER(thread_list_head);

#if USE_AESD_CHAR_DEVICE
static bool parse_seekto_command(const char *line, size_t len, struct aesd_seekto *seekto)
{
    static const char prefix[] = "AESDCHAR_IOCSEEKTO:";
    size_t prefix_len = strlen(prefix);
    char tmp[128];
    size_t copy_len;
    unsigned int x, y;

    if (len <= prefix_len || strncmp(line, prefix, prefix_len) != 0) {
        return false;
    }

    copy_len = (len < sizeof(tmp) - 1) ? len : sizeof(tmp) - 1;
    memcpy(tmp, line, copy_len);
    tmp[copy_len] = '\0';

    if (sscanf(tmp + prefix_len, "%u,%u", &x, &y) != 2) {
        return false;
    }

    seekto->write_cmd = x;
    seekto->write_cmd_offset = y;
    return true;
}
#endif

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
    pthread_mutex_lock(&file_mutex);
    FILE *file = fopen(FILE_PATH, "a");
    if (file == NULL) {
        perror("Error opening or creating file");
        syslog(LOG_ERR, "Failed to open file: %s", FILE_PATH);
        pthread_mutex_unlock(&file_mutex);
        return -1;
    }
    size_t written = fwrite(data, 1, length, file);
    fclose(file);
    pthread_mutex_unlock(&file_mutex);
    return (written == length) ? 0 : -1;
}

void* timer_thread_function(void* arg) {
    (void)arg;
#if USE_AESD_CHAR_DEVICE
    while (keep_running) {
        sleep(1);
    }
    return NULL;
#else
    time_t rawtime;
    struct tm *timeinfo;
    char time_buf[100];
    char final_buf[150];

    while (keep_running) {
        for (int i = 0; i < 10 && keep_running; i++) {
            sleep(1);
        }
        
        if (!keep_running) break;

        time(&rawtime);
        timeinfo = localtime(&rawtime);

        if (strftime(time_buf, sizeof(time_buf), "%a, %d %b %Y %T %z", timeinfo) != 0) {
            int len = snprintf(final_buf, sizeof(final_buf), "timestamp: %s\n", time_buf);
            if (len > 0) {
                append_to_file(final_buf, len);
            }
        }
    }
    return NULL;
#endif
}

void* thread_function(void* thread_param) {
    thread_data_t *data = (thread_data_t *)thread_param;
    char read_buf[BUFFER_SIZE];
    char *packet_buf = NULL;
    size_t packet_len = 0;

    char ipstr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(data->client_addr.sin_addr), ipstr, sizeof(ipstr));
    syslog(LOG_INFO, "Accepted connection from %s", ipstr);

    while (keep_running) {
        ssize_t bytes_received = recv(data->client_fd, read_buf, BUFFER_SIZE, 0);
        if (bytes_received > 0) {
            char *new_ptr = realloc(packet_buf, packet_len + bytes_received);
            if (new_ptr == NULL) {
                perror("Memory allocation error (realloc). Packet discarded.");
                free(packet_buf);
                break;
            }
            packet_buf = new_ptr;
            memcpy(packet_buf + packet_len, read_buf, bytes_received);
            packet_len += bytes_received;

            size_t start_idx = 0;
            for (size_t i = 0; i < packet_len; i++) {
                if (packet_buf[i] == '\n') {
                    size_t current_packet_size = (i - start_idx) + 1;
                    char *line = packet_buf + start_idx;
                    bool handled_as_seek = false;

            #if USE_AESD_CHAR_DEVICE
                    struct aesd_seekto seekto;
                    if (parse_seekto_command(line, current_packet_size, &seekto)) {
                        handled_as_seek = true;

                        pthread_mutex_lock(&file_mutex);

                        /* Requirement 1.5: single fd used for BOTH the ioctl and the
                        * subsequent read, so f_pos set by the ioctl is honored */
                        int fd = open(FILE_PATH, O_RDWR);
                        if (fd < 0) {
                            perror("Error opening aesdchar device for ioctl");
                            syslog(LOG_ERR, "Failed to open %s for ioctl: %s", FILE_PATH, strerror(errno));
                        } else {
                            if (ioctl(fd, AESDCHAR_IOCSEEKTO, &seekto) != 0) {
                                syslog(LOG_ERR, "ioctl AESDCHAR_IOCSEEKTO failed: %s", strerror(errno));
                            } else {
                                /* Requirement 1.4: send device content back over the
                                * socket, starting from the seeked position */
                                char send_buf[BUFFER_SIZE];
                                ssize_t bytes_read;
                                while ((bytes_read = read(fd, send_buf, BUFFER_SIZE)) > 0) {
                                    send(data->client_fd, send_buf, bytes_read, 0);
                                }
                            }
                            close(fd);
                        }

                        pthread_mutex_unlock(&file_mutex);
                    }
            #endif

                    if (!handled_as_seek) {
                        /* Requirement 1.3 (implicitly satisfied): only reached for
                        * normal lines, never for AESDCHAR_IOCSEEKTO commands */
                        append_to_file(line, current_packet_size);

                        pthread_mutex_lock(&file_mutex);
                        FILE *file = fopen(FILE_PATH, "r");
                        if (file != NULL) {
                            char send_buf[BUFFER_SIZE];
                            size_t bytes_read;
                            while ((bytes_read = fread(send_buf, 1, BUFFER_SIZE, file)) > 0) {
                                send(data->client_fd, send_buf, bytes_read, 0);
                            }
                            fclose(file);
                        }
                        pthread_mutex_unlock(&file_mutex);
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
    close(data->client_fd);
    syslog(LOG_INFO, "Closed connection from %s", ipstr);
    data->is_complete = 1;
    return thread_param;
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

    pthread_t timer_thread;
    if (pthread_create(&timer_thread, NULL, timer_thread_function, NULL) != 0) {
        perror("Timer thread creation error");
        close(server_fd);
        closelog();
        return -1;
    }

    if (listen(server_fd, 10) < 0) {
        perror("Socket listen error");
        close(server_fd);
        closelog();
        return -1;
    }

    SLIST_INIT(&thread_list_head);
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    while (keep_running) {
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_fd < 0) {
            if (!keep_running) break; 
            continue;
        }

        thread_data_t *node = malloc(sizeof(thread_data_t));
        if (node == NULL) {
            perror("Failed to allocate memory for thread context");
            close(client_fd);
            continue;
        }

        node->client_fd = client_fd;
        node->client_addr = client_addr;
        node->is_complete = 0;

        if (pthread_create(&(node->thread_id), NULL, thread_function, node) != 0) {
            perror("pthread_create failure");
            close(client_fd);
            free(node);
            continue;
        }

        SLIST_INSERT_HEAD(&thread_list_head, node, entries);

        thread_data_t *thread_curr;
        thread_data_t *thread_temp;
        SLIST_FOREACH_SAFE(thread_curr, &thread_list_head, entries, thread_temp) {
            if (thread_curr->is_complete) {
                pthread_join(thread_curr->thread_id, NULL);
                SLIST_REMOVE(&thread_list_head, thread_curr, thread_data, entries);
                free(thread_curr);
            }
        }
    }

    if (server_fd != -1) {
        close(server_fd);
    }

    pthread_join(timer_thread, NULL);

    thread_data_t *thread_curr;
    while (!SLIST_EMPTY(&thread_list_head)) {
        thread_curr = SLIST_FIRST(&thread_list_head);
        pthread_join(thread_curr->thread_id, NULL);
        SLIST_REMOVE_HEAD(&thread_list_head, entries);
        free(thread_curr);
    }

    pthread_mutex_destroy(&file_mutex);
#if !USE_AESD_CHAR_DEVICE
    unlink(FILE_PATH);
#endif
    closelog();
    return 0;
}