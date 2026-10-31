#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVER_PORT 5000
#define MAX_PDU 128  /* Maximale Anfragelaenge inklusive LF. */
#define MAX_MESSAGES 20
#define MAX_TEXT 100

/* Gemeinsamer Verlauf fuer alle Clients dieses Serverprozesses.
   Zusaetzliches Byte je Nachricht fuer das Stringende '\0'. */
static char messages[MAX_MESSAGES][MAX_TEXT + 1];
static int message_count = 0;

/* send() kann weniger Bytes senden als angefordert. */
static int send_all(int fd, const char *text)
{
    size_t length = strlen(text);
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(fd, text + sent, length - sent,
                         MSG_NOSIGNAL);

        if (n == -1) {
            if (errno == EINTR)
                continue;  /* Nach Signalunterbrechung erneut versuchen. */

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

/* Wird nur mit einer vollstaendigen, LF-beendeten PDU aufgerufen. */
static void process_request(int fd, char *request, size_t length)
{
    char response[MAX_PDU];

    /* Nur druckbares ASCII zulassen; LF steht bereits am Ende.
       Dadurch werden auch eingebettete Nullbytes ausgeschlossen. */
    for (size_t i = 0; i < length - 1; i++) {
        unsigned char c = (unsigned char)request[i];

        if (c < 32 || c > 126) {
            send_all(fd, "ERR BAD_REQUEST\n");
            return;
        }
    }

    /* LF durch Stringende ersetzen: Stringfunktionen sind jetzt sicher. */
    request[length - 1] = '\0';

    if (strcmp(request, "PING") == 0) {
        snprintf(response, sizeof(response), "OK PONG\n");

    } else if (strcmp(request, "COUNT") == 0) {
        snprintf(response, sizeof(response),
                 "OK COUNT %d\n", message_count);

    } else if (strncmp(request, "ADD ", 4) == 0) {
        const char *text = request + 4;
        size_t text_length = strlen(text);

        if (text_length == 0 || text_length > MAX_TEXT) {
            snprintf(response, sizeof(response),
                     "ERR BAD_MESSAGE\n");
        } else if (message_count == MAX_MESSAGES) {
            snprintf(response, sizeof(response),
                     "ERR LIST_FULL\n");
        } else {
            /* Erst den Eintrag kopieren, danach die Anzahl erhoehen. */
            memcpy(messages[message_count], text, text_length + 1);
            message_count++;

            snprintf(response, sizeof(response),
                     "OK ADDED %d\n", message_count);
        }

    } else if (strncmp(request, "GET ", 4) == 0) {
        const char *number = request + 4;
        char *end;

        errno = 0;
        long index = strtol(number, &end, 10);

        /* Nur eine vorhandene, positive dezimale Nummer akzeptieren. */
        if (errno != 0 ||
            number[0] < '0' || number[0] > '9' ||
            *end != '\0' ||
            index < 1 || index > message_count) {
            snprintf(response, sizeof(response),
                     "ERR BAD_INDEX\n");
        } else {
            /* Protokollnummer 1 entspricht Arrayposition 0. */
            snprintf(response, sizeof(response),
                     "OK MSG %d %.100s\n",
                     (int)index, messages[index - 1]);
        }

    } else {
        snprintf(response, sizeof(response),
                 "ERR UNKNOWN_COMMAND\n");
    }

    if (send_all(fd, response) == 0)
        printf("Antwort: %s", response);
}

/* Eine Anfrage empfangen und beantworten.
   Byteweiser Empfang vereinfacht hier die Erkennung des Zeilenendes. */
static void handle_client(int fd)
{
    char request[MAX_PDU];
    size_t used = 0;

    while (used < sizeof(request)) {
        ssize_t n = recv(fd, &request[used], 1, 0);

        if (n == -1) {
            if (errno == EINTR)
                continue;

            perror("recv");
            return;
        }

        if (n == 0) {
            /* Ohne LF liegt keine vollstaendige Anfrage vor. */
            if (used > 0)
                fprintf(stderr, "Unvollstaendige Anfrage verworfen\n");
            return;
        }

        used++;

        if (request[used - 1] == '\n') {
    process_request(fd, request, used);
    return;
}
}

    /* 128 Bytes ohne LF: Eine gueltige PDU passt nicht mehr hinein. */
    if (send_all(fd, "ERR LINE_TOO_LONG\n") == 0)
        printf("Zu lange Anfrage abgewiesen\n");
}

int main(void)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    /* Erleichtert den Neustart nach vorherigen TCP-Verbindungen. */
    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) == -1) {
        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(server_fd, (const struct sockaddr *)&server_addr,
             sizeof(server_addr)) == -1) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, 5) == -1) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Server bereit auf 127.0.0.1:%d\n", SERVER_PORT);

    /* Clients nacheinander bedienen; pro Verbindung eine Anfrage. */
    for (;;) {
        int client_fd = accept(server_fd, NULL, NULL);

        if (client_fd == -1) {
            if (errno == EINTR)
                continue;

            perror("accept");
            break;
        }

        printf("Verbindung angenommen\n");
        handle_client(client_fd);

        if (close(client_fd) == -1)
            perror("close client");
    }

    if (close(server_fd) == -1)
        perror("close server");

    return EXIT_FAILURE;
}
