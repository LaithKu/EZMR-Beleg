#define _POSIX_C_SOURCE 200809L
/* Praktikum Sockets d: gleichberechtigte Peers unter Linux.
 * Bauen: gcc -std=c11 -Wall -Wextra -Wpedantic -g -pthread peer.c -o peer
 * Start: ./peer 5001
 * Eingabe: 127.0.0.1 5002 ADD Hallo von Peer A
 * Ende: QUIT (oder EOF). Jede Instanz besitzt ihre eigene fluechtige Liste.
 * Nur der Empfangsthread greift auf die Liste zu: kein Listen-Mutex noetig.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_PDU 128
#define MAX_MESSAGES 20
#define MAX_TEXT 100
#define TIMEOUT_SECONDS 5

static char messages[MAX_MESSAGES][MAX_TEXT + 1];
static int message_count = 0;
static atomic_int stopping = 0; /* Gemeinsames Stoppsignal fuer beide Threads. */

static void close_socket(int fd)
{
    if (close(fd) == -1)
        perror("close");
}

static int parse_port(const char *text, unsigned short *port)
{
    if (*text == '\0' || strspn(text, "0123456789") != strlen(text))
        return -1;
    errno = 0;
    long value = strtol(text, NULL, 10);
    if (errno != 0 || value < 1 || value > 65535)
        return -1;
    *port = (unsigned short)value;
    return 0;
}

/* Unter Linux begrenzt SO_SNDTIMEO auch das blockierende connect(). */
static int set_timeouts(int fd)
{
    struct timeval timeout = {.tv_sec = TIMEOUT_SECONDS, .tv_usec = 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) == -1 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout)) == -1) {
        perror("setsockopt timeout");
        return -1;
    }
    return 0;
}

/* Unveraendertes Prinzip aus a/b: Teiluebertragungen vervollstaendigen. */
static int send_all(int fd, const char *text)
{
    size_t length = strlen(text);
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = send(fd, text + sent, length - sent, MSG_NOSIGNAL);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            perror("send");
            return -1;
        }
        if (n == 0) {
            fprintf(stderr, "send: kein Fortschritt\n");
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static long long monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Maximal 128 Byte inklusive LF, plus lokales Stringende im Zielpuffer.
 * Rueckgabe: Laenge > 0, 0 bei EOF ohne Daten, -2 bei zu langer PDU,
 * -1 bei Fehler/Timeout/unvollstaendiger PDU.
 * Die Frist gilt fuer die GESAMTE PDU, auch bei langsamem Byteversand.
 */
static int receive_line(int fd, char line[MAX_PDU + 1])
{
    size_t used = 0;
    long long deadline = monotonic_ms() + TIMEOUT_SECONDS * 1000;
    while (used < MAX_PDU) {
        long long remaining = deadline - monotonic_ms();
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int ready = poll(&pfd, 1, (int)remaining);
        if (ready == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (ready == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        ssize_t n = recv(fd, &line[used], 1, 0);
        if (n == -1) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return -1;
        }
        if (n == 0) {
            if (used == 0)
                return 0;
            errno = EPROTO;
            return -1;
        }
        if (line[used++] == '\n') {
            line[used] = '\0';
            return (int)used;
        }
    }
    return -2;
}

/* Listenoperationen aus c. Ausschliesslich im Empfangsthread aufgerufen.
 * ADD aendert nur den Zustand des angesprochenen Peers, nicht aller Peers.
 */
static void process_request(int fd, char *request, size_t length)
{
    char response[MAX_PDU];
    for (size_t i = 0; i < length - 1; i++) {
        unsigned char c = (unsigned char)request[i];
        if (c < 32 || c > 126) {
            send_all(fd, "ERR BAD_REQUEST\n");
            return;
        }
    }
    request[length - 1] = '\0';
    if (strcmp(request, "PING") == 0) {
        snprintf(response, sizeof(response), "OK PONG\n");
    } else if (strcmp(request, "COUNT") == 0) {
        snprintf(response, sizeof(response), "OK COUNT %d\n", message_count);
    } else if (strncmp(request, "ADD ", 4) == 0) {
        const char *text = request + 4;
        size_t length_text = strlen(text);
        if (length_text == 0 || length_text > MAX_TEXT) {
            snprintf(response, sizeof(response), "ERR BAD_MESSAGE\n");
        } else if (message_count >= MAX_MESSAGES) {
            snprintf(response, sizeof(response), "ERR LIST_FULL\n");
        } else {
            memcpy(messages[message_count], text, length_text + 1);
            message_count++;
            snprintf(response, sizeof(response), "OK ADDED %d\n", message_count);
        }
    } else if (strncmp(request, "GET ", 4) == 0) {
        const char *number = request + 4;
        char *end;
        errno = 0;
        long index = strtol(number, &end, 10);
        if (errno != 0 || number[0] < '0' || number[0] > '9' ||
            *end != '\0' || index < 1 || index > message_count) {
            snprintf(response, sizeof(response), "ERR BAD_INDEX\n");
        } else {
            snprintf(response, sizeof(response), "OK MSG %d %.100s\n",
                     (int)index, messages[index - 1]);
        }
    } else {
        snprintf(response, sizeof(response), "ERR UNKNOWN_COMMAND\n");
    }
    send_all(fd, response); /* send_all meldet Fehler; kein automatisches ADD-Retry. */
}

/* Empfangsteil: nimmt unabhaengig von der Konsoleneingabe Anfragen an.
 * Nichtblockierendes accept() + poll() erlauben regelmaessige Stopppruefung.
 */
static void *serve_connections(void *argument)
{
    int listener = *(int *)argument;
    while (!atomic_load(&stopping)) {
        struct pollfd pfd = {.fd = listener, .events = POLLIN};
        int ready = poll(&pfd, 1, 200);
        if (ready == -1) {
            if (errno == EINTR)
                continue;
            perror("poll listener");
            exit(EXIT_FAILURE);
        }
        if (ready == 0 || atomic_load(&stopping))
            continue;
        int fd = accept(listener, NULL, NULL);
        if (fd == -1) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ECONNABORTED)
                continue;
            perror("accept");
            exit(EXIT_FAILURE);
        }
        if (set_timeouts(fd) == 0) {
            char request[MAX_PDU + 1];
            int length = receive_line(fd, request);
            if (length > 0)
                process_request(fd, request, (size_t)length);
            else if (length == -2)
                send_all(fd, "ERR LINE_TOO_LONG\n");
            else if (length == -1)
                perror("eingehende Anfrage");
        }
        close_socket(fd);
    }
    return NULL;
}

/* Sendeteil: lokale Liste wird NICHT direkt angefasst.
 * Auch Zugriff auf die eigene Liste erfolgt ueber den eigenen TCP-Port.
 */
static void request_peer(const char *ip, const char *port_text,
                         const char *command)
{
    unsigned short port;
    if (parse_port(port_text, &port) == -1) {
        fprintf(stderr, "Ungueltiger Zielport\n");
        return;
    }
    size_t length = strlen(command);
    if (length > MAX_PDU - 1 || strchr(command, '\r') != NULL) {
        fprintf(stderr, "Kommando: maximal 127 Byte, kein CR\n");
        return;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    int converted = inet_pton(AF_INET, ip, &address.sin_addr);
    if (converted != 1) {
        if (converted == 0)
            fprintf(stderr, "Ungueltige IPv4-Adresse\n");
        else
            perror("inet_pton");
        return;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return;
    }
    if (set_timeouts(fd) == -1) {
        close_socket(fd);
        return;
    }
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("connect");
        close_socket(fd);
        return;
    }
    char request[MAX_PDU + 1];
    memcpy(request, command, length);
    request[length] = '\n';
    request[length + 1] = '\0';
    if (send_all(fd, request) == 0) {
        char response[MAX_PDU + 1];
        int received = receive_line(fd, response);
        if (received > 0)
            fwrite(response, 1, (size_t)received, stdout);
        else if (received == -1)
            perror("Antwort");
        else
            fprintf(stderr, "Fehlende oder zu lange Antwort\n");
    }
    close_socket(fd);
}

int main(int argc, char *argv[])
{
    unsigned short own_port;
    if (argc != 2 || parse_port(argv[1], &own_port) == -1) {
        fprintf(stderr, "Aufruf: %s EIGENER_PORT (1..65535)\n", argv[0]);
        return EXIT_FAILURE;
    }
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }
    int reuse = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) == -1) {
        perror("setsockopt reuse");
        close_socket(listener);
        return EXIT_FAILURE;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(own_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (const struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("bind");
        close_socket(listener);
        return EXIT_FAILURE;
    }
    if (listen(listener, 5) == -1) {
        perror("listen");
        close_socket(listener);
        return EXIT_FAILURE;
    }
    int flags = fcntl(listener, F_GETFL);
    if (flags == -1 || fcntl(listener, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl");
        close_socket(listener);
        return EXIT_FAILURE;
    }
    pthread_t receiver;
    int error = pthread_create(&receiver, NULL, serve_connections, &listener);
    if (error != 0) {
        fprintf(stderr, "pthread_create: %s\n", strerror(error));
        close_socket(listener);
        return EXIT_FAILURE;
    }
    printf("Peer bereit auf 127.0.0.1:%u\n", (unsigned int)own_port);
    printf("Eingabe: ZIEL_IP ZIEL_PORT KOMMANDO | Ende: QUIT\n");
    char line[256];
    for (;;) {
        printf("peer> ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL)
            break;
        char *newline = strchr(line, '\n');
        if (newline == NULL) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF) {}
            fprintf(stderr, "Eingabe zu lang oder ohne Zeilenende\n");
            continue;
        }
        *newline = '\0';
        if (strcmp(line, "QUIT") == 0)
            break;
        if (*line == '\0')
            continue;
        char *state;
        char *ip = strtok_r(line, " \t", &state);
        char *port = strtok_r(NULL, " \t", &state);
        if (ip == NULL || port == NULL) {
            fprintf(stderr, "Format: ZIEL_IP ZIEL_PORT KOMMANDO\n");
            continue;
        }
        while (*state == ' ' || *state == '\t')
            state++;
        if (*state == '\0') {
            fprintf(stderr, "Kommando fehlt\n");
            continue;
        }
        request_peer(ip, port, state);
    }
    /* QUIT wartet auf den Empfangsthread, bevor der Listener geschlossen wird.
     * Laufende Empfangsoperationen enden spaetestens durch ihre Frist.
     */
    atomic_store(&stopping, 1);
    error = pthread_join(receiver, NULL);
    if (error != 0) {
        fprintf(stderr, "pthread_join: %s\n", strerror(error));
        return EXIT_FAILURE;
    }
    close_socket(listener);
    return EXIT_SUCCESS;
}
