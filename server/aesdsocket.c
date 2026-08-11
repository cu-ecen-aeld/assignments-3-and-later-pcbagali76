#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <signal.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024

static volatile sig_atomic_t caught_signal = 0;
static int server_fd = -1;
static int client_fd = -1;

static void signal_handler(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        caught_signal = 1;
        if (client_fd != -1) {
            shutdown(client_fd, SHUT_RDWR);
        }
        if (server_fd != -1) {
            shutdown(server_fd, SHUT_RDWR);
        }
    }
}

static void send_file_content(int cfd, FILE *df)
{
    fseek(df, 0, SEEK_SET);
    char buf[BUFFER_SIZE];
    size_t read_bytes;

    while ((read_bytes = fread(buf, 1, sizeof(buf), df)) > 0) {
        size_t total_sent = 0;
        while (total_sent < read_bytes) {
            ssize_t sent = send(cfd, buf + total_sent, read_bytes - total_sent, 0);
            if (sent < 0) {
                if (errno == EINTR && caught_signal) {
                    return;
                }
                syslog(LOG_ERR, "Send failed: %s", strerror(errno));
                return;
            }
            total_sent += sent;
        }
    }
}

int main(int argc, char *argv[])
{
    bool daemon_mode = false;
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = true;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        syslog(LOG_ERR, "Error setting up SIGINT handler: %s", strerror(errno));
        closelog();
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        syslog(LOG_ERR, "Error setting up SIGTERM handler: %s", strerror(errno));
        closelog();
        return -1;
    }

    struct addrinfo hints;
    struct addrinfo *res = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, PORT, &hints, &res) != 0) {
        syslog(LOG_ERR, "getaddrinfo failed");
        closelog();
        return -1;
    }

    server_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (server_fd < 0) {
        syslog(LOG_ERR, "Socket creation failed: %s", strerror(errno));
        freeaddrinfo(res);
        closelog();
        return -1;
    }

    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        syslog(LOG_ERR, "setsockopt SO_REUSEADDR failed: %s", strerror(errno));
        close(server_fd);
        freeaddrinfo(res);
        closelog();
        return -1;
    }

    if (bind(server_fd, res->ai_addr, res->ai_addrlen) < 0) {
        syslog(LOG_ERR, "Bind failed: %s", strerror(errno));
        close(server_fd);
        freeaddrinfo(res);
        closelog();
        return -1;
    }

    freeaddrinfo(res);
    res = NULL;

    if (daemon_mode) {
        if (daemon(0, 0) < 0) {
            syslog(LOG_ERR, "Daemonization failed: %s", strerror(errno));
            close(server_fd);
            closelog();
            return -1;
        }
    }

    if (listen(server_fd, 10) < 0) {
        syslog(LOG_ERR, "Listen failed: %s", strerror(errno));
        close(server_fd);
        closelog();
        return -1;
    }

    FILE *data_file = fopen(DATA_FILE, "a+");
    if (!data_file) {
        syslog(LOG_ERR, "Failed to open data file %s: %s", DATA_FILE, strerror(errno));
        close(server_fd);
        closelog();
        return -1;
    }

    while (!caught_signal) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (caught_signal) {
                break;
            }
            syslog(LOG_ERR, "Accept failed: %s", strerror(errno));
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, sizeof(client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        char recv_buf[BUFFER_SIZE];
        char *packet_buf = NULL;
        size_t packet_size = 0;
        ssize_t bytes_read;

        while (!caught_signal && (bytes_read = recv(client_fd, recv_buf, sizeof(recv_buf), 0)) > 0) {
            char *new_buf = realloc(packet_buf, packet_size + bytes_read);
            if (!new_buf) {
                syslog(LOG_ERR, "Memory realloc failed");
                free(packet_buf);
                packet_buf = NULL;
                packet_size = 0;
                break;
            }
            packet_buf = new_buf;
            memcpy(packet_buf + packet_size, recv_buf, bytes_read);
            packet_size += bytes_read;

            if (memchr(recv_buf, '\n', bytes_read) != NULL) {
                fwrite(packet_buf, 1, packet_size, data_file);
                fflush(data_file);

                free(packet_buf);
                packet_buf = NULL;
                packet_size = 0;

                send_file_content(client_fd, data_file);
            }
        }

        if (packet_buf) {
            free(packet_buf);
            packet_buf = NULL;
        }

        close(client_fd);
        client_fd = -1;
        syslog(LOG_INFO, "Closed connection from %s", client_ip);
    }

    if (data_file) {
        fclose(data_file);
    }

    if (client_fd != -1) {
        close(client_fd);
        client_fd = -1;
    }

    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }

    remove(DATA_FILE);

    if (caught_signal) {
        syslog(LOG_INFO, "Caught signal, exiting");
    }

    closelog();
    return 0;
}
