#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVER_PORT 5000
#define MAX_PDU 128  /* Maximale Anfragelaenge inklusive LF. */

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
            const char *response = "ERR UNKNOWN_COMMAND\n";

            /* Laenge und Inhalt vergleichen; kein C-String erforderlich. */
            if (used == 5 && memcmp(request, "PING\n", 5) == 0)
                response = "OK PONG\n";

            if (send_all(fd, response) == 0)
                printf("Antwort: %s", response);

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
