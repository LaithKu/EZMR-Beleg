#define _POSIX_C_SOURCE 200809L
/* IPC d: Jede Instanz sendet UND empfaengt Anfragen.
 * Bauen: gcc -std=c11 -Wall -Wextra -Wpedantic -g -pthread ipc_peer.c -o ipc_peer
 * Start: ./ipc_peer 1  und in anderem Terminal ./ipc_peer 2
 * Eingabe bei Peer 1: 2 ADD Hallo von A
 * Ende: QUIT, EOF, Strg+C oder SIGTERM. Eigene Queues werden entfernt.
 * Jede Instanz besitzt eine eigene, nicht replizierte Nachrichtenliste.
 */
#include "ipc_protocol.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <time.h>
#include <unistd.h>

#define PEER_KEY_BASE 0x45525000
#define MAX_PEER_ID 255
#define MAX_MESSAGES 20
#define MAX_TEXT 100

/* Nur der Empfangsthread liest/aendert diese Liste: keine konkurrierenden
 * Listenoperationen. Auch eigene Anfragen werden ueber die Queue gesendet.
 */
static char messages[MAX_MESSAGES][MAX_TEXT + 1];
static int message_count = 0;
static atomic_int stopping = 0;
/* Signalhandler darf keine interne Atomics-Sperre benoetigen. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Stoppsignal muss lockfrei sein");

static void handle_stop(int signal_number)
{
    (void)signal_number;
    atomic_store(&stopping, 1);
}

struct receive_buffer {
    long mtype;
    unsigned char bytes[IPC_PAYLOAD_SIZE + 1];
};
_Static_assert(offsetof(struct receive_buffer, bytes) == sizeof(long),
               "Unerwartetes Padding im Empfangspuffer");

static int remove_queue(int queue)
{
    if (msgctl(queue, IPC_RMID, NULL) == -1 &&
        errno != EINVAL && errno != EIDRM) {
        perror("Queue entfernen");
        return -1;
    }
    return 0;
}

static int parse_peer_id(const char *text)
{
    if (*text == '\0' || strspn(text, "0123456789") != strlen(text))
        return -1;
    errno = 0;
    long value = strtol(text, NULL, 10);
    return (errno != 0 || value < 1 || value > MAX_PEER_ID) ? -1 : (int)value;
}

/* Bekannte Listenoperationen aus c; Transport und Datenverwaltung getrennt. */
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

/* Listenoperationen entsprechen der Socket-Aufgabe c.
 * Eine Anfrage wird vollstaendig bearbeitet, bevor die naechste beginnt.
 */
static void execute_command(const char *command, char answer[IPC_TEXT_SIZE])
{
    if (!valid_text(command)) {
        snprintf(answer, IPC_TEXT_SIZE, "ERR BAD_REQUEST");
    } else if (strcmp(command, "PING") == 0) {
        snprintf(answer, IPC_TEXT_SIZE, "OK PONG");
    } else if (strcmp(command, "COUNT") == 0) {
        snprintf(answer, IPC_TEXT_SIZE, "OK COUNT %d", message_count);
    } else if (strncmp(command, "ADD ", 4) == 0) {
        const char *text = command + 4;
        size_t length = strlen(text);

        if (length == 0 || length > MAX_TEXT) {
            snprintf(answer, IPC_TEXT_SIZE, "ERR BAD_MESSAGE");
        } else if (message_count >= MAX_MESSAGES) {
            snprintf(answer, IPC_TEXT_SIZE, "ERR LIST_FULL");
        } else {
            /* Erst Inhalt speichern, dann neuen Eintrag sichtbar machen. */
            memcpy(messages[message_count], text, length + 1);
            message_count++;
            snprintf(answer, IPC_TEXT_SIZE, "OK ADDED %d", message_count);
        }
    } else if (strncmp(command, "GET ", 4) == 0) {
        const char *number = command + 4;
        char *end;
        errno = 0;
        long index = strtol(number, &end, 10);

        if (errno != 0 || number[0] < '0' || number[0] > '9' ||
            *end != '\0' || index < 1 || index > message_count) {
            snprintf(answer, IPC_TEXT_SIZE, "ERR BAD_INDEX");
        } else {
            /* Nummerierung im Protokoll ab 1, im Array ab 0. */
            snprintf(answer, IPC_TEXT_SIZE, "OK MSG %d %.100s",
                     (int)index, messages[index - 1]);
        }
    } else {
        snprintf(answer, IPC_TEXT_SIZE, "ERR UNKNOWN_COMMAND");
    }
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

    execute_command(request->text, response.payload.text);

    /* Ein verschwundener Client oder eine volle Antwortqueue darf den
     * Server nicht blockieren. Fehler melden; keine automatische Wiederholung.
     */
    int result;
    do {
        result = msgsnd(request->reply_queue, &response,
                        IPC_PAYLOAD_SIZE, IPC_NOWAIT);
    } while (result == -1 && errno == EINTR && !atomic_load(&stopping));

    if (result == -1)
        perror("msgsnd Antwort");

}

/* Maximal 50 Empfangsversuche mit jeweils 100 ms Pause.
 * Begrenzte Wartezeit statt endlosem Blockieren bei ausgefallenem Server.
 */
static int receive_reply(int queue, struct ipc_payload *reply)
{
    const struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000L};
    for (int attempt = 0; attempt < 50 && !atomic_load(&stopping); attempt++) {
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
    errno = atomic_load(&stopping) ? EINTR : ETIMEDOUT;
    return -1;
}


/* Empfangsthread: Anfragen nacheinander bearbeiten, unabhaengig davon,
 * ob der Hauptthread gerade auf Tastatureingaben oder eine Antwort wartet.
 */
static void *receive_requests(void *argument)
{
    int queue = *(int *)argument;
    const struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000L};
    while (!atomic_load(&stopping)) {
        struct receive_buffer incoming;
        ssize_t length = msgrcv(queue, &incoming, sizeof(incoming.bytes),
                                0, IPC_NOWAIT | MSG_NOERROR);
        if (length == -1) {
            if (errno == EINTR)
                continue;
            if (errno == ENOMSG) {
                if (nanosleep(&pause, NULL) == -1 && errno != EINTR) {
                    perror("nanosleep Empfang");
                    atomic_store(&stopping, 1);
                }
                continue;
            }
            perror("Empfangsqueue");
            atomic_store(&stopping, 1);
            break;
        }
        if (incoming.mtype != IPC_REQUEST_TYPE ||
            length != (ssize_t)IPC_PAYLOAD_SIZE) {
            fprintf(stderr, "Ungueltige Anfrage-PDU verworfen\n");
            continue;
        }
        struct ipc_payload request;
        memcpy(&request, incoming.bytes, sizeof(request));
        process_request(queue, &request);
    }
    return NULL;
}

/* Sendeteil aus b: pro Anfrage eine temporaere Rueckantwortqueue.
 * Kein automatisches Wiederholen von ADD: Antwortverlust kann einen bereits
 * gespeicherten Eintrag betreffen; blindes Wiederholen wuerde ihn duplizieren.
 */
static void send_request(int target_id, const char *command, unsigned int id)
{
    if (strlen(command) >= IPC_TEXT_SIZE) {
        fprintf(stderr, "Kommando zu lang (maximal 127 ASCII-Zeichen)\n");
        return;
    }
    int target = msgget((key_t)(PEER_KEY_BASE + target_id), 0);
    if (target == -1) {
        fprintf(stderr, "Zielpeer %d nicht erreichbar: %s\n",
                target_id, strerror(errno));
        return;
    }
    int reply_queue = msgget(IPC_PRIVATE, 0600);
    if (reply_queue == -1) {
        perror("Rueckantwortqueue");
        return;
    }
    struct ipc_message request = {0};
    request.mtype = IPC_REQUEST_TYPE;
    request.payload.reply_queue = reply_queue;
    request.payload.request_id = id;
    memcpy(request.payload.text, command, strlen(command) + 1);
    if (!valid_text(request.payload.text)) {
        fprintf(stderr, "Nur druckbares ASCII ohne Zeilenumbrueche erlaubt\n");
        goto cleanup;
    }
    if (msgsnd(target, &request, IPC_PAYLOAD_SIZE, IPC_NOWAIT) == -1) {
        perror("msgsnd Anfrage");
        goto cleanup;
    }
    struct ipc_payload reply;
    if (receive_reply(reply_queue, &reply) == -1) {
        perror("Antwort");
        goto cleanup;
    }
    if (reply.request_id != id || reply.reply_queue != reply_queue ||
        !valid_text(reply.text)) {
        fprintf(stderr, "Ungueltige Antwort oder falsche Zuordnung\n");
        goto cleanup;
    }
    printf("%s\n", reply.text);

cleanup:
    if (remove_queue(reply_queue) == -1)
        atomic_store(&stopping, 1);
}

int main(int argc, char *argv[])
{
    int own_id = argc == 2 ? parse_peer_id(argv[1]) : -1;
    if (own_id == -1) {
        fprintf(stderr, "Aufruf: %s PEER_ID (1..255)\n", argv[0]);
        return EXIT_FAILURE;
    }
    struct sigaction action = {0};
    action.sa_handler = handle_stop;
    if (sigemptyset(&action.sa_mask) == -1 ||
        sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) {
        perror("Signalbehandlung");
        return EXIT_FAILURE;
    }
    /* poll() soll auch bei umgeleiteter Eingabe alle Zeilen erkennen. */
    if (setvbuf(stdin, NULL, _IONBF, 0) != 0) {
        fprintf(stderr, "Eingabepuffer konnte nicht konfiguriert werden\n");
        return EXIT_FAILURE;
    }
    key_t key = (key_t)(PEER_KEY_BASE + own_id);
    int queue = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
    if (queue == -1) {
        perror("Eigene Anfragequeue");
        fprintf(stderr, "Peer-ID bereits belegt oder alte Queue vorhanden? ipcs -q pruefen.\n");
        return EXIT_FAILURE;
    }
    pthread_t receiver;
    int error = pthread_create(&receiver, NULL, receive_requests, &queue);
    if (error != 0) {
        fprintf(stderr, "pthread_create: %s\n", strerror(error));
        remove_queue(queue);
        return EXIT_FAILURE;
    }
    printf("Peer %d bereit: Key=0x%08lx, Queue-ID=%d\n",
           own_id, (unsigned long)key, queue);
    printf("Eingabe: ZIEL_ID KOMMANDO | Ende: QUIT\n");
    unsigned int request_id = 1;
    int status = EXIT_SUCCESS;
    char line[256];
    while (!atomic_load(&stopping)) {
        printf("peer> ");
        fflush(stdout);
        /* Periodische Stopppruefung auch bei ausbleibender Konsoleneingabe. */
        int ready = 0;
        while (!atomic_load(&stopping) && ready == 0) {
            struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
            ready = poll(&input, 1, 100);
            if (ready == -1 && errno == EINTR)
                ready = 0;
        }
        if (atomic_load(&stopping))
            break;
        if (ready == -1) {
            perror("poll Eingabe");
            status = EXIT_FAILURE;
            break;
        }
        if (fgets(line, sizeof(line), stdin) == NULL) {
            if (ferror(stdin) && !atomic_load(&stopping)) {
                perror("Konsoleneingabe");
                status = EXIT_FAILURE;
            }
            break;
        }
        char *newline = strchr(line, '\n');
        if (newline == NULL) {
            int c;
            while (!atomic_load(&stopping) &&
                   (c = getchar()) != '\n' && c != EOF) {}
            fprintf(stderr, "Eingabe zu lang oder ohne Zeilenende\n");
            continue;
        }
        *newline = '\0';
        if (strcmp(line, "QUIT") == 0)
            break;
        if (*line == '\0')
            continue;
        char *state;
        char *id_text = strtok_r(line, " \t", &state);
        int target_id = id_text == NULL ? -1 : parse_peer_id(id_text);
        if (target_id == -1) {
            fprintf(stderr, "Ziel-ID muss zwischen 1 und 255 liegen\n");
            continue;
        }
        while (*state == ' ' || *state == '\t')
            state++;
        if (*state == '\0') {
            fprintf(stderr, "Format: ZIEL_ID KOMMANDO\n");
            continue;
        }
        send_request(target_id, state, request_id++);
    }
    /* Erst Empfangsthread beenden, dann seine Queue entfernen.
     * Fremde Anfragequeues werden niemals entfernt.
     */
    atomic_store(&stopping, 1);
    error = pthread_join(receiver, NULL);
    if (error != 0) {
        fprintf(stderr, "pthread_join: %s\n", strerror(error));
        status = EXIT_FAILURE;
    }
    if (remove_queue(queue) == -1)
        status = EXIT_FAILURE;
    else
        printf("Eigene Anfragequeue entfernt.\n");
    return status;
}
