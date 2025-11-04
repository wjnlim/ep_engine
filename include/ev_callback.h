#ifndef EV_CALLBACK_H
#define EV_CALLBACK_H

#include <sys/queue.h>

#include "ep_engine/epoll_event_engine.h"

// typedef void (*event_cb_fn)(Epolled_fd* epdfd, void* arg);
typedef void (*on_done_cb)(Epolled_fd* epdfd, void* arg);

typedef struct EV_callback {
    on_event_cb cb_fn;
    void* cb_arg;
    Epdfd_state epdfd_state;

    on_done_cb on_done;
    void* on_done_arg;
    
    Epolled_fd* epdfd;
// #ifdef DEBUG_EXEC
//     char* name;
// #endif

    TAILQ_ENTRY(EV_callback) cb_link;
} EV_callback;

#endif