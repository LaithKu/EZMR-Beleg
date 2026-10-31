#define _POSIX_C_SOURCE 200809L
/* IPC a: System-V-Message-Queue-Server.
 * Bauen: gcc -std=c11 -Wall -Wextra -Wpedantic -g ipc_server.c -o ipc_server
 * Start: ./ipc_server        Kontrolle: ipcs -q
 * Ende: Strg+C oder SIGTERM; eigene Queue wird entfernt.
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

/* Im Signalhandler keine Ausgabe und keine Queue-Verwaltung ausfuehren. */
static void handle_stop(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

/* Ein zusaetzliches Byte erlaubt, ueberlange PDUs zu erkennen und zu
 * entfernen. Ohne diese Behandlung koennte E2BIG die Queue blockieren.
 */
struct receive_buffer {
    long mtype;
    unsigned char bytes[IPC_PAYLOAD_SIZE + 1];
};
_Static_assert(offsetof(struct receive_buffer, bytes) == sizeof(long),
               "Unerwartetes Padding im Empfangspuffer");

static int valid_text(const char text[IPC_TEXT_SIZE])
{
    const char *end = memchr(text, '\0', IPC_TEXT_SIZE);
    if (end == NULL)
        return 0;
    for (const char *p = text; p < end; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c > 126)
            return 0;
    }
    return 1;
}

static void process_request(int server_queue, const struct ipc_payload *request)
{
    /* Auch Queue-ID 0 ist gueltig. Antwort nie in eigene Anfragequeue legen. */
    if (request->reply_queue < 0 || request->reply_queue == server_queue) {
        fprintf(stderr, "Anfrage verworfen: ungueltige Rueckantwortqueue\n");
        return;
    }

    struct ipc_message response = {0};
    response.mtype = IPC_RESPONSE_TYPE;
    response.payload.reply_queue = request->reply_queue;
    response.payload.request_id = request->request_id;

    const char *answer;
    if (!valid_text(request->text))
        answer = "ERR BAD_REQUEST";
    else if (strcmp(request->text, "PING") == 0)
        answer = "OK PONG";
    else
        answer = "ERR UNKNOWN_COMMAND";

    snprintf(response.payload.text, sizeof(response.payload.text), "%s", answer);

    /* Ein verschwundener Client oder eine volle Antwortqueue darf den
     * Server nicht blockieren. Fehler melden; keine automatische Wiederholung.
     */
    int result;
    do {
        result = msgsnd(request->reply_queue, &response,
                        IPC_PAYLOAD_SIZE, IPC_NOWAIT);
    } while (result == -1 && errno == EINTR && !stopping);

    if (result == -1)
        perror("msgsnd Antwort");
    else
        printf("Anfrage %u -> Queue %d: %s\n",
               request->request_id, request->reply_queue, answer);
}

int main(void)
{
    struct sigaction action = {0};
    action.sa_handler = handle_stop;
    if (sigemptyset(&action.sa_mask) == -1 ||
        sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) {
        perror("Signalbehandlung");
        return EXIT_FAILURE;
    }

    /* IPC_EXCL schuetzt vor zweitem Server und unbemerkter Uebernahme
     * einer alten Queue. 0600: Lesen/Schreiben nur fuer den Eigentuemer.
     */
    int queue = msgget(IPC_SERVER_KEY, IPC_CREAT | IPC_EXCL | 0600);
    if (queue == -1) {
        perror("msgget");
        fprintf(stderr, "Bei vorhandener Queue: laufenden Server bzw. ipcs -q pruefen.\n");
        return EXIT_FAILURE;
    }
    printf("IPC-Server bereit: Key=0x%08lx, Queue-ID=%d\n",
           (unsigned long)IPC_SERVER_KEY, queue);
    printf("Nutzdaten pro Nachricht: %zu Byte; Ende mit Strg+C\n", IPC_PAYLOAD_SIZE);

    int status = EXIT_SUCCESS;
    const struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000L};
    while (!stopping) {
        struct receive_buffer incoming;
        /* Typ 0 liest die aelteste Nachricht unabhaengig vom Typ.
         * Typ und Groesse werden anschliessend validiert.
         */
        ssize_t length = msgrcv(queue, &incoming, sizeof(incoming.bytes),
                                0, IPC_NOWAIT | MSG_NOERROR);
        if (length == -1) {
            if (errno == EINTR)
                continue;
            if (errno == ENOMSG) {
                /* 100-ms-Polling verhindert aktives Warten und ermoeglicht
                 * ein zuverlaessiges Beenden ohne Signal/Warte-Rennen.
                 * Keine harte Echtzeitgarantie; zusaetzliche Pollinglatenz.
                 */
                if (nanosleep(&pause, NULL) == -1 && errno != EINTR) {
                    perror("nanosleep");
                    status = EXIT_FAILURE;
                    break;
                }
                continue;
            }
            perror("msgrcv");
            status = EXIT_FAILURE;
            break;
        }
        if (incoming.mtype != IPC_REQUEST_TYPE ||
            length != (ssize_t)IPC_PAYLOAD_SIZE) {
            fprintf(stderr, "PDU verworfen: Typ=%ld, Nutzdaten=%zd Byte\n",
                    incoming.mtype, length);
            continue;
        }

        struct ipc_payload request;
        memcpy(&request, incoming.bytes, sizeof(request));
        process_request(queue, &request);
    }

    /* System-V-Queues verschwinden nicht automatisch mit dem Prozess. */
    if (msgctl(queue, IPC_RMID, NULL) == -1) {
        if (errno != EINVAL && errno != EIDRM) {
            perror("msgctl IPC_RMID");
            status = EXIT_FAILURE;
        } else {
            printf("Anfragequeue wurde bereits entfernt.\n");
        }
    } else {
        printf("Anfragequeue %d entfernt.\n", queue);
    }
    return status;
}
