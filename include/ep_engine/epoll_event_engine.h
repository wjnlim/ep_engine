#ifndef EPOLL_EVENT_ENGINE_H
#define EPOLL_EVENT_ENGINE_H

#include <sys/types.h>
#include <stdbool.h>
#include <pthread.h>

typedef struct EP_engine EP_engine;
typedef struct EV_callback EV_callback;
typedef struct Epolled_fd Epolled_fd;
typedef enum Epdfd_state {
    EPDFD_READABLE,
    EPDFD_WRITABLE,
    EPDFD_RD_SHUTDOWN,
    EPDFD_WR_SHUTDOWN
} Epdfd_state;

typedef void (*on_event_cb)(Epolled_fd* epdfd, void* arg, Epdfd_state state);


EP_engine* ep_engine_create(bool use_thread_pool, int n_workers);
void ep_engine_destroy(EP_engine* engine);

// pthread_t ep_engine_start_event_loop(EP_engine* engine);
int ep_engine_start_event_loop(EP_engine* engine);
void ep_engine_stop_event_loop(EP_engine* engine);

Epolled_fd* epdfd_create(int fd, EP_engine* engine);

void epdfd_shutdown(Epolled_fd* epdfd, bool shutdown_rd, bool shutdown_wr);
void epdfd_destroy(Epolled_fd* epdfd, void (*on_done)(void* arg), void* on_done_arg);

int epdfd_get_fd(Epolled_fd* epdfd);
EP_engine* epdfd_get_engine(Epolled_fd* epdfd);
/*
    Register read event callback.
    The callback will be called when the epdfd becomes ready
    (or is already ready) to read

    (This func must be called after handling read event ( ex) read until EAGAIN) 
    in a read ev handler callback to get event notification again.)
*/
void epdfd_notify_on_readable(Epolled_fd* epdfd, 
                                on_event_cb on_readable_cb, void* cb_arg);

/*
    Register write event callback.
    The callback will be called when the epdfd becomes ready 
    (or is already ready) to write

    (This func must be called after handling write event ( ex) write until EAGAIN or
    write out all data) 
    in a write ev handler callback to get event notification again.)
*/
void epdfd_notify_on_writable(Epolled_fd* epdfd, 
                                on_event_cb on_writable_cb, void* cb_arg);


/*
    Forcibly set the epdfd ready to read and schedule the
    registered callback
*/
void epdfd_set_readable(Epolled_fd* epdfd);
/*
    Forcibly set the epdfd ready to write and schedule the
    registered callback
*/
void epdfd_set_writable(Epolled_fd* epdfd);



#endif
