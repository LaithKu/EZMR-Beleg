#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

#include <stddef.h>
#include <sys/ipc.h>

/* Frei gewaehlter Schluessel fuer die gemeinsame Anfragequeue.
 * Schluessel (key) und vom Kernel vergebene Queue-ID sind verschieden.
 */
#define IPC_SERVER_KEY ((key_t)0x45524d01)
#define IPC_TEXT_SIZE 128
#define IPC_REQUEST_TYPE 1L
#define IPC_RESPONSE_TYPE 2L

/* Lokales Protokoll: beide Programme verwenden diesen Header und dieselbe ABI.
 * text: maximal 127 druckbare ASCII-Zeichen + '\0', KEIN LF erforderlich.
 * request_id: wird in der Antwort unveraendert zurueckgegeben.
 * reply_queue: Queue-ID der vom Client angelegten Rueckantwortqueue.
 */
struct ipc_payload {
    int reply_queue;
    unsigned int request_id;
    char text[IPC_TEXT_SIZE];
};

/* System V verlangt ein positives long als erstes Feld. */
struct ipc_message {
    long mtype;
    struct ipc_payload payload;
};

/* msgsnd/msgrcv: Die Groesse enthaelt NICHT das fuehrende long! */
#define IPC_PAYLOAD_SIZE (sizeof(struct ipc_payload))
_Static_assert(offsetof(struct ipc_message, payload) == sizeof(long),
               "Unerwartetes Padding zwischen Typ und Nutzdaten");

#endif
