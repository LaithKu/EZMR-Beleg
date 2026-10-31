#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_PDU 128

/* Wie beim Server: Auch Teiluebertragungen beruecksichtigen. */
static int send_all(int fd, const char *text)
{
    size_t length = strlen(text);
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(fd, text + sent, length - sent,
                         MSG_NOSIGNAL);

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

/* Eine vollstaendige Antwort lesen und ausgeben. */
static int receive_response(int fd)
{
    char response[MAX_PDU];
    size_t used = 0;

    while (used < sizeof(response)) {
        ssize_t n = recv(fd, &response[used], 1, 0);

        if (n == -1) {
            if (errno == EINTR)
                continue;

            perror("recv");
            return -1;
        }

        if (n == 0) {
            fprintf(stderr,
                    "Server hat ohne vollstaendige Antwort geschlossen\n");
            return -1;
        }

        used++;

        if (response[used - 1] == '\n') {
            /* Ausgabe anhand der Laenge, ohne Nullterminierung. */
            fwrite(response, 1, used, stdout);
            return 0;
        }
    }

    fprintf(stderr, "Antwort ueberschreitet die Empfangsgrenze\n");
    return -1;
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr, "Aufruf: %s IPv4 PORT KOMMANDO\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Port vollstaendig als Zahl einlesen und Wertebereich pruefen. */
    char *end;
    errno = 0;
    long port = strtol(argv[2], &end, 10);

    if (errno != 0 || end == argv[2] || *end != '\0' ||
        port < 1 || port > 65535) {
        fprintf(stderr, "Ungueltiger Port: erlaubt sind 1 bis 65535\n");
        return EXIT_FAILURE;
    }

    /* LF wird vom Client angehaengt: maximal 127 Byte Kommando. */
    size_t command_length = strlen(argv[3]);

    if (command_length > MAX_PDU - 1 ||
        strchr(argv[3], '\n') != NULL ||
        strchr(argv[3], '\r') != NULL) {
        fprintf(stderr,
                "Kommando: maximal 127 Byte, keine Zeilenumbrueche\n");
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((unsigned short)port);

    /* Textadresse in die binaere IPv4-Darstellung umwandeln. */
    int converted = inet_pton(AF_INET, argv[1],
                              &server_addr.sin_addr);

    if (converted != 1) {
        if (converted == 0)
            fprintf(stderr, "Ungueltige IPv4-Adresse\n");
        else
            perror("inet_pton");

        return EXIT_FAILURE;
    }

    /* Ein zusaetzliches Byte fuer das lokale Stringende '\0'. */
    char request[MAX_PDU + 1];
    memcpy(request, argv[3], command_length);
    request[command_length] = '\n';
    request[command_length + 1] = '\0';

    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int result = EXIT_FAILURE;

    if (connect(fd, (const struct sockaddr *)&server_addr,
                sizeof(server_addr)) == -1) {
        perror("connect");
    } else if (send_all(fd, request) == 0 &&
               receive_response(fd) == 0) {
        result = EXIT_SUCCESS;
    }

    /* Socket sowohl nach Erfolg als auch nach Fehlern schliessen. */
    if (close(fd) == -1) {
        perror("close");
        result = EXIT_FAILURE;
    }

    return result;
}
