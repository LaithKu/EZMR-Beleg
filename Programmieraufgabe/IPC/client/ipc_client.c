#define _POSIX_C_SOURCE 200809L
/* IPC b: Ein Kommando senden und eine Antwort ausgeben.
 * Bauen: gcc -std=c11 -Wall -Wextra -Wpedantic -g ipc_client.c -o ipc_client
 * Aufruf: ./ipc_client PING   oder spaeter: ./ipc_client "ADD Hallo"
 */
#include "ipc_protocol.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <time.h>

static volatile sig_atomic_t stopping = 0;

static void stop_client(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

/* Maximal 50 Empfangsversuche mit jeweils 100 ms Pause.
 * Begrenzte Wartezeit statt endlosem Blockieren bei ausgefallenem Server.
 */
static int receive_reply(int queue, struct ipc_payload *reply)
{
    const struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000L};
    for (int attempt = 0; attempt < 50 && !stopping; attempt++) {
        struct {
            long mtype;
            unsigned char bytes[IPC_PAYLOAD_SIZE + 1];
        } incoming;
        ssize_t length = msgrcv(queue, &incoming, sizeof(incoming.bytes),
                                0, IPC_NOWAIT | MSG_NOERROR);
        if (length >= 0) {
            if (incoming.mtype != IPC_RESPONSE_TYPE ||
                length != (ssize_t)IPC_PAYLOAD_SIZE) {
                errno = EPROTO;
                return -1;
            }
            memcpy(reply, incoming.bytes, sizeof(*reply));
            return 0;
        }
        if (errno == EINTR)
            continue;
        if (errno != ENOMSG)
            return -1;
        if (nanosleep(&pause, NULL) == -1 && errno != EINTR)
            return -1;
    }
    errno = stopping ? EINTR : ETIMEDOUT;
    return -1;
}

int main(int argc, char *argv[])
{
    if (argc != 2 || strlen(argv[1]) >= IPC_TEXT_SIZE) {
        fprintf(stderr, "Aufruf: %s KOMMANDO (maximal 127 Zeichen)\n", argv[0]);
        return EXIT_FAILURE;
    }
    for (const unsigned char *p = (const unsigned char *)argv[1]; *p; p++) {
        if (*p < 32 || *p > 126) {
            fprintf(stderr, "Nur druckbares ASCII ohne Zeilenumbrueche erlaubt\n");
            return EXIT_FAILURE;
        }
    }
    struct sigaction action = {0};
    action.sa_handler = stop_client;
    if (sigemptyset(&action.sa_mask) == -1 ||
        sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) {
        perror("Signalbehandlung");
        return EXIT_FAILURE;
    }
    /* Die Serverqueue oeffnen, aber nicht selbst anlegen. */
    int server = msgget(IPC_SERVER_KEY, 0);
    if (server == -1) {
        perror("Serverqueue: zuerst ipc_server starten");
        return EXIT_FAILURE;
    }
    /* Pro Client-Aufruf eine neue Queue fuer die eigene Antwort. */
    int reply_queue = msgget(IPC_PRIVATE, 0600);
    if (reply_queue == -1) {
        perror("Rueckantwortqueue");
        return EXIT_FAILURE;
    }
    int result = EXIT_FAILURE;
    struct ipc_message request = {0};
    request.mtype = IPC_REQUEST_TYPE;
    request.payload.reply_queue = reply_queue;
    request.payload.request_id = 1; /* Genau eine Anfrage pro Client-Aufruf. */
    memcpy(request.payload.text, argv[1], strlen(argv[1]) + 1);

    if (msgsnd(server, &request, IPC_PAYLOAD_SIZE, IPC_NOWAIT) == -1) {
        perror("msgsnd");
        goto cleanup;
    }
    struct ipc_payload reply;
    if (receive_reply(reply_queue, &reply) == -1) {
        perror("msgrcv Antwort");
        goto cleanup;
    }
    if (reply.request_id != request.payload.request_id ||
        reply.reply_queue != reply_queue ||
        memchr(reply.text, '\0', sizeof(reply.text)) == NULL) {
        fprintf(stderr, "Ungueltige Antwort oder falsche Anfragenummer\n");
        goto cleanup;
    }
    printf("%s\n", reply.text);
    result = EXIT_SUCCESS; /* Auch eine ERR-Antwort ist ein erfolgter Austausch. */

cleanup:
    /* Nur die eigene Rueckantwortqueue entfernen, niemals die Serverqueue. */
    if (msgctl(reply_queue, IPC_RMID, NULL) == -1) {
        perror("Rueckantwortqueue entfernen");
        result = EXIT_FAILURE;
    }
    return result;
}
