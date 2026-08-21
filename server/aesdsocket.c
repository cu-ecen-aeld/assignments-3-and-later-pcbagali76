/*
 * aesdsocket.c - Multithreaded socket server for AESD Assignment 6
 *
 * Accepts multiple simultaneous connections, each handled by a new thread.
 * Writes to /var/tmp/aesdsocketdata are mutex-protected.
 * Appends RFC 2822 timestamps every 10 seconds via timer_create().
 * Gracefully exits on SIGINT/SIGTERM, joining all threads.
 *
 * Uses SLIST (sys/queue.h) for thread management.
 */

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
#include <pthread.h>
#include <sys/queue.h>
#include <time.h>

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024
#define TIMESTAMP_INTERVAL_SEC 10

/* --------------- Global State --------------- */

static volatile sig_atomic_t caught_signal = 0;
static int server_fd = -1;
static pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;
static timer_t timerid;

/* --------------- Thread Linked List --------------- */

typedef struct thread_entry {
    pthread_t thread;
    int client_fd;
    bool thread_complete;
    char client_ip[INET_ADDRSTRLEN];
    SLIST_ENTRY(thread_entry) entries;
} thread_entry_t;

SLIST_HEAD(thread_list_head, thread_entry) thread_list = SLIST_HEAD_INITIALIZER(thread_list);

/* --------------- Signal Handler --------------- */

static void signal_handler(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        caught_signal = 1;
        /* Unblock the accept() call by shutting down the server socket */
        if (server_fd != -1) {
            shutdown(server_fd, SHUT_RDWR);
        }
    }
}

/* --------------- Thread Function --------------- */

/*
 * send_file_content_locked - Send entire data file content to client.
 * Caller must hold data_mutex.
 */
static void send_file_content_locked(int cfd)
{
    FILE *df = fopen(DATA_FILE, "r");
    if (!df) {
        syslog(LOG_ERR, "Failed to open data file for reading: %s", strerror(errno));
        return;
    }

    char buf[BUFFER_SIZE];
    size_t read_bytes;

    while ((read_bytes = fread(buf, 1, sizeof(buf), df)) > 0) {
        size_t total_sent = 0;
        while (total_sent < read_bytes) {
            ssize_t sent = send(cfd, buf + total_sent, read_bytes - total_sent, 0);
            if (sent < 0) {
                if (errno == EINTR && caught_signal) {
                    fclose(df);
                    return;
                }
                syslog(LOG_ERR, "Send failed: %s", strerror(errno));
                fclose(df);
                return;
            }
            total_sent += sent;
        }
    }

    fclose(df);
}

/*
 * thread_func - Per-connection thread entry point.
 * Receives data from client, writes complete packets (newline-terminated)
 * to the data file under mutex, then sends back the full file content.
 */
static void *thread_func(void *arg)
{
    thread_entry_t *entry = (thread_entry_t *)arg;
    int cfd = entry->client_fd;

    char recv_buf[BUFFER_SIZE];
    char *packet_buf = NULL;
    size_t packet_size = 0;
    ssize_t bytes_read;

    while (!caught_signal && (bytes_read = recv(cfd, recv_buf, sizeof(recv_buf), 0)) > 0) {
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

        /* Check if we received a complete packet (newline-terminated) */
        if (memchr(recv_buf, '\n', bytes_read) != NULL) {
            /* Lock mutex, write to file, read back full file, unlock */
            pthread_mutex_lock(&data_mutex);

            FILE *df = fopen(DATA_FILE, "a");
            if (df) {
                fwrite(packet_buf, 1, packet_size, df);
                fclose(df);
            } else {
                syslog(LOG_ERR, "Failed to open data file for writing: %s", strerror(errno));
            }

            /* Send full file content back to client while still holding the lock */
            send_file_content_locked(cfd);

            pthread_mutex_unlock(&data_mutex);

            free(packet_buf);
            packet_buf = NULL;
            packet_size = 0;
        }
    }

    if (packet_buf) {
        free(packet_buf);
        packet_buf = NULL;
    }

    close(cfd);
    syslog(LOG_INFO, "Closed connection from %s", entry->client_ip);

    entry->thread_complete = true;
    return NULL;
}

/* --------------- Timestamp Timer --------------- */

/*
 * timer_handler - Called every 10 seconds by timer_create's SIGEV_THREAD.
 * Appends an RFC 2822 formatted timestamp to the data file under mutex.
 */
static void timer_handler(union sigval sv)
{
    (void)sv;

    if (caught_signal) {
        return;
    }

    time_t now;
    struct tm tm_buf;
    char time_str[128];

    time(&now);
    localtime_r(&now, &tm_buf);

    /* RFC 2822 compliant format */
    size_t len = strftime(time_str, sizeof(time_str), "timestamp:%a, %d %b %Y %T %z\n", &tm_buf);
    if (len == 0) {
        syslog(LOG_ERR, "strftime failed");
        return;
    }

    pthread_mutex_lock(&data_mutex);

    FILE *df = fopen(DATA_FILE, "a");
    if (df) {
        fwrite(time_str, 1, len, df);
        fclose(df);
    } else {
        syslog(LOG_ERR, "Failed to open data file for timestamp: %s", strerror(errno));
    }

    pthread_mutex_unlock(&data_mutex);
}

/*
 * init_timer - Set up a repeating 10-second timer using timer_create with SIGEV_THREAD.
 * Returns 0 on success, -1 on failure.
 */
static int init_timer(void)
{
    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = timer_handler;
    sev.sigev_notify_attributes = NULL;
    sev.sigev_value.sival_ptr = NULL;

    if (timer_create(CLOCK_REALTIME, &sev, &timerid) != 0) {
        syslog(LOG_ERR, "timer_create failed: %s", strerror(errno));
        return -1;
    }

    struct itimerspec its;
    its.it_value.tv_sec = TIMESTAMP_INTERVAL_SEC;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = TIMESTAMP_INTERVAL_SEC;
    its.it_interval.tv_nsec = 0;

    if (timer_settime(timerid, 0, &its, NULL) != 0) {
        syslog(LOG_ERR, "timer_settime failed: %s", strerror(errno));
        return -1;
    }

    return 0;
}

/* --------------- Cleanup Helpers --------------- */

/*
 * join_completed_threads - Iterate the linked list and join threads
 * that have marked themselves complete. Remove their entries.
 */
static void join_completed_threads(void)
{
    thread_entry_t *entry;
    thread_entry_t *tmp;

    /* Use manual iteration so we can safely remove during traversal */
    entry = SLIST_FIRST(&thread_list);
    thread_entry_t *prev = NULL;
    while (entry != NULL) {
        tmp = SLIST_NEXT(entry, entries);
        if (entry->thread_complete) {
            pthread_join(entry->thread, NULL);
            if (prev == NULL) {
                SLIST_REMOVE_HEAD(&thread_list, entries);
            } else {
                SLIST_NEXT(prev, entries) = tmp;
            }
            free(entry);
        } else {
            prev = entry;
        }
        entry = tmp;
    }
}

/*
 * join_all_threads - Join all remaining threads (used during shutdown).
 */
static void join_all_threads(void)
{
    while (!SLIST_EMPTY(&thread_list)) {
        thread_entry_t *entry = SLIST_FIRST(&thread_list);
        pthread_join(entry->thread, NULL);
        SLIST_REMOVE_HEAD(&thread_list, entries);
        free(entry);
    }
}

/* --------------- Main --------------- */

int main(int argc, char *argv[])
{
    bool daemon_mode = false;
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = true;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    /* Set up signal handlers for SIGINT and SIGTERM */
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

    /* Resolve address and create socket */
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

    /* Daemonize after bind, before listen (as per assignment requirements) */
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

    /* Initialize the 10-second periodic timestamp timer (in parent/main thread) */
    if (init_timer() != 0) {
        syslog(LOG_ERR, "Failed to initialize timer");
        close(server_fd);
        closelog();
        return -1;
    }

    /* Main accept loop - blocks on accept, spawns thread per connection */
    while (!caught_signal) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
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

        /* Create a new thread entry and add to the linked list */
        thread_entry_t *new_entry = malloc(sizeof(thread_entry_t));
        if (!new_entry) {
            syslog(LOG_ERR, "Failed to allocate thread entry: %s", strerror(errno));
            close(client_fd);
            continue;
        }

        new_entry->client_fd = client_fd;
        new_entry->thread_complete = false;
        strncpy(new_entry->client_ip, client_ip, sizeof(new_entry->client_ip));
        new_entry->client_ip[sizeof(new_entry->client_ip) - 1] = '\0';

        /* Insert into linked list before creating thread */
        SLIST_INSERT_HEAD(&thread_list, new_entry, entries);

        /* Spawn the connection handler thread */
        int rc = pthread_create(&new_entry->thread, NULL, thread_func, new_entry);
        if (rc != 0) {
            syslog(LOG_ERR, "pthread_create failed: %s", strerror(rc));
            SLIST_REMOVE(&thread_list, new_entry, thread_entry, entries);
            close(client_fd);
            free(new_entry);
            continue;
        }

        /* After spawning a new thread, clean up any completed threads */
        join_completed_threads();
    }

    /* --------------- Graceful Shutdown --------------- */

    syslog(LOG_INFO, "Caught signal, exiting");

    /* Stop the timer */
    timer_delete(timerid);

    /* Close the server socket to prevent new connections */
    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }

    /* Wait for all threads to finish */
    join_all_threads();

    /* Clean up the data file */
    remove(DATA_FILE);

    /* Destroy the mutex */
    pthread_mutex_destroy(&data_mutex);

    closelog();
    return 0;
}
