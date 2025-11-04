
#define _GNU_SOURCE
#include <sys/epoll.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/queue.h>
#include <string.h>

#include "utils/my_err.h"
#include "ev_callback.h"
#include "ev_cb_thread_pool.h"
#include "utils/socket_helper.h"
#include "ep_engine/epoll_event_engine.h"

#define MAX_EPOLL_EVENTS 128

#define DEFAULT_N_FREE_CBS 128

typedef struct Epolled_fd Epolled_fd;

typedef TAILQ_HEAD(EV_cb_q, EV_callback) EV_cb_q;
typedef TAILQ_HEAD(Epolled_fd_q, Epolled_fd) Epolled_fd_q;

typedef enum Event_type {
    RD,
    WR
} Event_type;

/*
    EV_HANDLER_ARMED: The handler is armed to be scheculed 
                when an I/O event(became readable or writable) occurs.
    EV_NOT_AVAILABLE: There is no available I/O event 
    (no event has occurred or an event is handled(scheduled))
    EV_AVAILABLE: There is an I/O event which is ready to be handled(scheduled)
*/
typedef enum Event_state {
    EV_HANDLER_ARMED,
    EV_NOT_AVAILABLE, 
    EV_AVAILABLE,
    EV_CLOSED
} Event_state;

typedef struct Event {
    Event_type type;
    Event_state state;
    EV_callback* ev_handler;
    Epolled_fd* epdfd;
} Event;


struct Epolled_fd {
    int fd;
    EP_engine* engine;
    int epoll_fd; // epoll fd which is polling the fd

    uint32_t interest_ev;
    
    Event rev;
    Event wev;

    int ref_cnt;

    bool rd_closed;
    bool wr_closed;

    bool destroying;

    void (*on_destroy_cb)(void* arg);
    void* on_destroy_arg;
    pthread_mutex_t lock;

    TAILQ_ENTRY(Epolled_fd) fd_link;
};

struct EP_engine {
    int epoll_fd;
    int shutdown_evfd;
    int kick_evfd;

    struct {
        EV_cb_q curr_list;
        EV_cb_q pending_list;
    } ev_callbacks;

    struct {
        Epolled_fd_q curr_list;
        Epolled_fd_q pending_list;
    } destroy_fd_q;

    EV_cb_thread_pool* thread_pool;

    pthread_t ev_loop_thread;
    bool running_loop;
    bool shutting_down;

    pthread_cond_t cv;
    pthread_mutex_t lock;


    EV_cb_q free_cb_pool;
};

// static void* event_loop(void* arg);
static void event_loop(EP_engine* engine);
static void schedule_cb(EP_engine* engine, EV_callback* cb, Epolled_fd* epdfd);
static EV_callback* ep_engine_get_free_cb(EP_engine* engine);
static EV_callback* ep_engine_get_free_cb_locked(EP_engine* engine);
static int write_eventfd(int eventfd);
static int read_eventfd(int eventfd);

static void event_init(Event* ev, Event_type type, Event_state state,
                                    EV_callback* evcb, Epolled_fd* epdfd) {
    ev->type = type;
    ev->state = state;
    ev->ev_handler = evcb;
    ev->epdfd = epdfd;
}

/* epoll_ funcs */
static void epoll_add_fd(int epoll_fd, int fd,  uint32_t events, 
                                            Epolled_fd* epdfd) {
    struct epoll_event ee;
    ee.events = events;
    if (epdfd) {
        ee.data.ptr = epdfd;
    } else {
        ee.data.fd = fd;
    }

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ee) < 0) {
        err_exit_sys("[epoll_event_engine.c: epoll_add_fd()] epoll_ctl() error");
    }
}

static void epoll_modify_fd(int epoll_fd, int fd,  uint32_t events, 
                                            Epolled_fd* epdfd) {
    struct epoll_event ee;
    ee.events = events;
    if (epdfd) {
        ee.data.ptr = epdfd;
    } else {
        ee.data.fd = fd;
    }

    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ee) < 0) {
        err_exit_sys("[epoll_event_engine.c: epoll_modify_fd()] epoll_ctl() error");
    }
}

static void epoll_del_fd(int epoll_fd, int fd) {
    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        err_exit_sys("[epoll_event_engine.c: epoll_del_fd()] epoll_ctl() error");
    }
}

/* free_cb_pool_ funcs */
static void free_cb_pool_destroy(EV_cb_q* pool) {
    EV_callback *evcb, *next;
    evcb = TAILQ_FIRST(pool);
    while (evcb != NULL) {
        next = TAILQ_NEXT(evcb, cb_link);
        free(evcb);
        evcb = next;
    }
    TAILQ_INIT(pool);
}

static int free_cb_pool_init(EV_cb_q* pool, int n_free_cbs) {
    TAILQ_INIT(pool);

    int n;
    EV_callback* evcb;
    for (n = 0; n < n_free_cbs; n++) {
        evcb = (EV_callback*)malloc(sizeof(*evcb));
        if (evcb == NULL) {
            // return -1;
            break;
        }
        TAILQ_INSERT_TAIL(pool, evcb, cb_link);
    }

    if (n < n_free_cbs) {
        err_msg_sys("[epoll_event_engine.c: free_cb_pool_init()] malloc() error");
        free_cb_pool_destroy(pool);
        return -1;
    }

    return 0;
}

static void evcb_init(EV_callback* evcb, on_event_cb cb_fn, void* cb_arg,
                            on_done_cb on_done, void* on_done_arg, Epolled_fd* epdfd) {
    evcb->cb_fn = cb_fn;
    evcb->cb_arg = cb_arg;
    evcb->on_done = on_done;
    evcb->on_done_arg = on_done_arg;
    evcb->epdfd = epdfd;
}

static void evcb_free(EV_callback* evcb, EP_engine* engine) {
    evcb_init(evcb, NULL, NULL, NULL, NULL, NULL);
    pthread_mutex_lock(&engine->lock);
    TAILQ_INSERT_TAIL(&engine->free_cb_pool, evcb, cb_link);
    pthread_mutex_unlock(&engine->lock);
}

/* epdfd_ funcs */

Epolled_fd* epdfd_create(int fd, EP_engine* engine) {
    Epolled_fd* epdfd = (Epolled_fd*)malloc(sizeof(*epdfd));
    if (epdfd == NULL) {
        err_msg_sys("[epoll_event_engine.c: epdfd_create()] malloc() error");
        return NULL;
    }

    epdfd->fd = fd;
    epdfd->engine = engine;
    epdfd->epoll_fd = engine->epoll_fd;
    epdfd->interest_ev = (EPOLLIN | EPOLLOUT | EPOLLET);


    event_init(&epdfd->rev, RD, EV_NOT_AVAILABLE, NULL, epdfd);
    event_init(&epdfd->wev, WR, EV_NOT_AVAILABLE, NULL, epdfd);
    epdfd->ref_cnt = 1;

    epdfd->rd_closed = false;
    epdfd->wr_closed = false;

    epdfd->destroying = false;
    epdfd->on_destroy_cb = NULL;
    epdfd->on_destroy_arg = NULL;
    pthread_mutex_init(&epdfd->lock, NULL);

    epoll_add_fd(epdfd->epoll_fd, fd, epdfd->interest_ev, epdfd);

    return epdfd;
}

#ifdef DEBUG_ENGINE
static const char* ev_state_to_str(Event_state state) {
    if (state == EV_HANDLER_ARMED) {
        return "EV_HANDLER_ARMED";
    } else if (state == EV_NOT_AVAILABLE) {
        return "EV_NOT_AVAILABLE";
    } else if (state == EV_AVAILABLE) {
        return "EV_AVAILABLE";
    } else if (state == EV_CLOSED) {
        return "EV_CLOSED";
    } else {
        return "UNKNOWNSTATE";
    }
}
#endif

static void event_close(Event* ev) {
    Epolled_fd* epdfd = ev->epdfd;

    Event_state curr_state = ev->state;
    switch (curr_state) {
        case EV_HANDLER_ARMED: {
            ev->state = EV_CLOSED;
            ev->ev_handler->epdfd_state = (ev->type == RD) ? EPDFD_RD_SHUTDOWN : EPDFD_WR_SHUTDOWN;
            schedule_cb(epdfd->engine, ev->ev_handler, epdfd);
            break;
        }
        case EV_NOT_AVAILABLE: {
        #ifdef DEBUG_ENGINE
            printf("[epoll_event_engine.c: event_close()] The fd(%d)'s ev state(%s): %s -> %s\n",
                epdfd->fd, (ev->type == RD) ? "RD" : "WR", ev_state_to_str(curr_state), "EV_CLOSED");
        #endif
            ev->state = EV_CLOSED;
            break;
        }
        case EV_AVAILABLE: {
        #ifdef DEBUG_ENGINE
            printf("[epoll_event_engine.c: event_close()] The fd(%d)'s ev state(%s): %s -> %s\n",
                epdfd->fd, (ev->type == RD) ? "RD" : "WR", ev_state_to_str(curr_state), "EV_CLOSED");
        #endif
            ev->state = EV_CLOSED;
            break;
        }
        case EV_CLOSED: {
            break;
        }
        default: {
            err_exit("[epoll_event_engine.c: event_close()] "
                        "the fd(%s) is in unknown state.", epdfd->fd);
        }
    }

}

static void event_close_locked(Event* ev) {
    Epolled_fd* epdfd = ev->epdfd;
    pthread_mutex_lock(&epdfd->lock);
    event_close(ev);
    pthread_mutex_unlock(&epdfd->lock);
}

static void epdfd_shutdown_(Epolled_fd* epdfd, bool shutdown_rd, bool shutdown_wr) {

    if (shutdown_rd && !epdfd->rd_closed) {
    #ifdef DEBUG_ENGINE
        printf("[epoll_event_engine.c: epdfd_shutdown()] Shuttig down(SHUT_RD) fd(%d)...\n", epdfd->fd);
    #endif
        shutdown(epdfd->fd, SHUT_RD);
        epdfd->rd_closed = true;
        event_close(&epdfd->rev);
    }
    if (shutdown_wr && !epdfd->wr_closed) {
    #ifdef DEBUG_ENGINE
        printf("[epoll_event_engine.c: epdfd_shutdown()] Shuttig down(SHUT_WR) fd(%d)...\n", epdfd->fd);
    #endif
        shutdown(epdfd->fd, SHUT_WR);
        epdfd->wr_closed = true;
        event_close(&epdfd->wev);
    }
}

void epdfd_shutdown(Epolled_fd* epdfd, bool shutdown_rd, bool shutdown_wr) {
    pthread_mutex_lock(&epdfd->lock);

    epdfd_shutdown_(epdfd, shutdown_rd, shutdown_wr);

    pthread_mutex_unlock(&epdfd->lock);
}


static void epdfd_unref(Epolled_fd* epdfd) {
    epdfd->ref_cnt--;
    if (epdfd->ref_cnt == 0) {
        pthread_mutex_lock(&epdfd->engine->lock);
        TAILQ_INSERT_TAIL(&epdfd->engine->destroy_fd_q.pending_list, epdfd, fd_link);
        pthread_mutex_unlock(&epdfd->engine->lock);

        write_eventfd(epdfd->engine->kick_evfd);
    }
}

static void epdfd_unref_locked(Epolled_fd* epdfd) {
    pthread_mutex_lock(&epdfd->lock);
    epdfd_unref(epdfd);
    pthread_mutex_unlock(&epdfd->lock);
}

void epdfd_destroy(Epolled_fd* epdfd, void (*on_done)(void* arg), void* on_done_arg) {
#ifdef DEBUG_ENGINE
    printf("[epoll_event_engine.c: epdfd_destroy()] Destroying fd(%d)...\n", epdfd->fd);
#endif
    pthread_mutex_lock(&epdfd->lock);
    if (epdfd->destroying) {
        pthread_mutex_unlock(&epdfd->lock);
        return;
    }

    epdfd->destroying = true;

    epdfd_shutdown_(epdfd, true, true);


    epdfd->on_destroy_cb = on_done;
    epdfd->on_destroy_arg = on_done_arg;

    epoll_del_fd(epdfd->epoll_fd, epdfd->fd);
    close(epdfd->fd);
    epdfd_unref(epdfd);

    pthread_mutex_unlock(&epdfd->lock);
}

int epdfd_get_fd(Epolled_fd* epdfd) {
    return epdfd->fd;
}

EP_engine* epdfd_get_engine(Epolled_fd* epdfd) {
    return epdfd->engine;
}

static int write_eventfd(int eventfd) {
    uint64_t u;
    ssize_t s;
    u = 1;
    do {
        s = write(eventfd, &u, sizeof(uint64_t));
    } while (s < 0 && errno == EINTR);
    if (s != sizeof(uint64_t)) {
        err_msg_sys("[epoll_event_engine.c: write_eventfd()] write() err.");
        return -1;
    }
    return 0;
}

static int read_eventfd(int eventfd) {
    uint64_t u;
    ssize_t s;
    do {
        s = read(eventfd, &u, sizeof(uint64_t));
    } while (s < 0 && errno == EINTR);
    if (s != sizeof(uint64_t)) {
        err_msg_sys("[epoll_event_engine.c: read_eventfd()] read() error");
        return -1;
    }

    return 0;
}

static void callback_done(Epolled_fd* epdfd, void* arg) {

    EP_engine* engine = epdfd->engine;
    EV_callback* cb = (EV_callback*)arg;
#ifdef DEBUG_ENGINE
    printf("[epoll_event_engine.c: callback_done()] Cb(%p) done\n", cb->cb_fn);
#endif

    epdfd_unref_locked(epdfd);
    evcb_free(cb, engine);
}

static void schedule_cb(EP_engine* engine, EV_callback* cb, Epolled_fd* epdfd) {
#ifdef DEBUG_ENGINE
    printf("[epoll_event_engine.c: schedule_cb()] schedule cb(%p)\n", cb->cb_fn);
#endif
    epdfd->ref_cnt++;
    pthread_mutex_lock(&engine->lock);
    TAILQ_INSERT_TAIL(&engine->ev_callbacks.pending_list, cb, cb_link);
    pthread_mutex_unlock(&engine->lock);
    // kick event loop
    write_eventfd(engine->kick_evfd);
}

static void notify_on_event(Event* ev, on_event_cb cb, void* cb_arg) {
    assert(cb != NULL);
    Epolled_fd* epdfd = ev->epdfd;

// #ifdef DEBUG_ENGINE
//     printf("[epoll_event_engine.c: notify_on_event()] The fd(%d) notify on event.\n", epdfd->fd);
// #endif

    pthread_mutex_lock(&epdfd->lock);

// #ifdef DEBUG_ENGINE
//     printf("[epoll_event_engine.c: notify_on_event()] The fd(%d) is getting free cb.\n", epdfd->fd);
// #endif

    EV_callback* evcb = ep_engine_get_free_cb_locked(epdfd->engine);
    evcb_init(evcb, cb, cb_arg, callback_done, evcb, epdfd);

    Event_state curr_state = ev->state;
    switch (curr_state) {
        case EV_HANDLER_ARMED: {
            err_exit("[epoll_event_engine.c: notify_on_event()] "
                         "the fd(%d) is already waiting on %s event.",
                         epdfd->fd, (ev->type == RD) ? "RD" : "WR");
            break;
        }
        case EV_NOT_AVAILABLE: {
            ev->ev_handler = evcb;
            ev->state = EV_HANDLER_ARMED;
        #ifdef DEBUG_ENGINE
            printf("[epoll_event_engine.c: notify_on_event()] The fd(%d)'s ev state(%s): %s -> %s\n",
                epdfd->fd, (ev->type == RD) ? "RD" : "WR", ev_state_to_str(curr_state), "EV_HANDLER_ARMED");
        #endif
            // }
            break;
        }
        case EV_AVAILABLE: {
            // ioev->handler = on_event_cb;
            ev->state = EV_NOT_AVAILABLE;
        #ifdef DEBUG_ENGINE
            printf("[epoll_event_engine.c: notify_on_event()] The fd(%d)'s ev state(%s): %s -> %s\n",
                epdfd->fd, (ev->type == RD) ? "RD" : "WR", ev_state_to_str(curr_state), "EV_NOT_AVAILABLE");
        #endif
            // schedule the handler
            evcb->epdfd_state = (ev->type == RD) ? EPDFD_READABLE : EPDFD_WRITABLE;
            schedule_cb(epdfd->engine, evcb, epdfd);
            break;
        }
        case EV_CLOSED: {
            // schedule the handler
            evcb->epdfd_state = (ev->type == RD) ? EPDFD_RD_SHUTDOWN : EPDFD_WR_SHUTDOWN;
            schedule_cb(epdfd->engine, evcb, epdfd);
            break;
        }
        default: {
            err_exit("[epoll_event_engine.c: notify_on_event()] "
                        "the fd(%s) is in unknown state.", epdfd->fd);
        }
    }

    pthread_mutex_unlock(&epdfd->lock);
}

void epdfd_notify_on_readable(Epolled_fd* epdfd, 
                                on_event_cb on_readable_cb, void* cb_arg) {
    notify_on_event(&epdfd->rev, on_readable_cb, cb_arg);
}

void epdfd_notify_on_writable(Epolled_fd* epdfd, 
                                on_event_cb on_writable_cb, void* cb_arg) {
    notify_on_event(&epdfd->wev, on_writable_cb, cb_arg);
}

static void event_set_available(Event* ev) {
    Epolled_fd* epdfd = ev->epdfd;

    pthread_mutex_lock(&epdfd->lock);

    Event_state curr_state = ev->state;
    switch (curr_state) {
        case EV_HANDLER_ARMED: {
            /*
                An event is consumed (scheduled) so set back to 'no available event'
            */
            ev->state = EV_NOT_AVAILABLE;
        #ifdef DEBUG_ENGINE
            printf("[epoll_event_engine.c: event_set_available()] The fd(%d)'s ev state(%s): %s -> %s\n",
                epdfd->fd, (ev->type == RD) ? "RD" : "WR", ev_state_to_str(curr_state), "EV_NOT_AVAILABLE");
        #endif
            ev->ev_handler->epdfd_state = (ev->type == RD) ? EPDFD_READABLE : EPDFD_WRITABLE;
            schedule_cb(epdfd->engine, ev->ev_handler, epdfd);
            break;
        }
        case EV_NOT_AVAILABLE: {
        /*
            An edge-triggered I/O event can occur
            between I/O functions(read/write() call and next read/write() call)
            in a I/O loop (this might be a spurious event)
            or between an I/O function(read()/write()) which returned EAGAIN
            and its EAGAIN handling routine (re-arming the fd).
            In these cases, if another thread's epoll_wait() reports the event
            then, mark the event 'has an I/O event' to let the EAGAIN handling
            routine know that there is an event to handle before re-arming the fd.
        */
            ev->state = EV_AVAILABLE;
        #ifdef DEBUG_ENGINE
            printf("[epoll_event_engine.c: event_set_available()] The fd(%d)'s ev state(%s): %s -> %s\n",
                epdfd->fd, (ev->type == RD) ? "RD" : "WR", ev_state_to_str(curr_state), "EV_AVAILABLE");
        #endif
            break;
        }
        case EV_AVAILABLE: {
            break;
        }
        case EV_CLOSED: {
            break;
        }
        default: {
            // Unknown state
            err_exit("[epoll_event_engine.c: event_set_available()] "
                        "the fd(%s) is in unknown state.", epdfd->fd);
            break;
        }
    }

    pthread_mutex_unlock(&epdfd->lock);
}

void epdfd_set_readable(Epolled_fd* epdfd) {
    event_set_available(&epdfd->rev);
}

void epdfd_set_writable(Epolled_fd* epdfd) {
    event_set_available(&epdfd->wev);
}


/* ep_engine_ funcs */
EP_engine* ep_engine_create(bool use_thread_pool, int n_workers) {
    EP_engine* engine = (EP_engine*)malloc(sizeof(*engine));
    if (engine == NULL) {
        err_msg_sys("[epoll_event_engine.c: ep_engine_create()] malloc() error");
        goto ALLOC_ENGINE_ERR;
    }
    // create epoll instance
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        err_msg_sys("[epoll_event_engine.c: ep_engine_create()] epoll_create1() error");
        goto EPOLL_CREATE_ERR;
    }
    engine->epoll_fd = epoll_fd;

    //init shutdown fd for epoll loop
    int shutdown_evfd = eventfd(0,0);
    if (shutdown_evfd == -1) {
        err_msg_sys("[epoll_event_engine.c: ep_engine_create()] eventfd(shutdown_fd) error");
        goto SHUTDOWN_EVFD_ERR;
    }
    engine->shutdown_evfd = shutdown_evfd;
    epoll_add_fd(epoll_fd, shutdown_evfd, EPOLLIN, NULL);

    int kick_evfd = eventfd(0,0);
    if (kick_evfd == -1) {
        err_msg_sys("[epoll_event_engine.c: ep_engine_create()] eventfd(kick_fd) error");
        goto KICK_EVFD_ERR;
    }
    engine->kick_evfd = kick_evfd;
    epoll_add_fd(epoll_fd, kick_evfd, EPOLLIN, NULL);

    TAILQ_INIT(&engine->ev_callbacks.curr_list);
    TAILQ_INIT(&engine->ev_callbacks.pending_list);

    TAILQ_INIT(&engine->destroy_fd_q.curr_list);
    TAILQ_INIT(&engine->destroy_fd_q.pending_list);

    engine->thread_pool = NULL;
    if (use_thread_pool) {
        engine->thread_pool = ev_cb_thread_pool_create(n_workers);
        if (engine->thread_pool == NULL) {
            err_msg("[epoll_event_engine.c: ep_engine_create()] ev_cb_thread_pool_create() error");
            goto THREAD_POOL_CREATE_ERR;
        }
    } 

    engine->running_loop = false;
    engine->shutting_down = false;

    pthread_cond_init(&engine->cv, NULL);
    pthread_mutex_init(&engine->lock, NULL);

    if (free_cb_pool_init(&engine->free_cb_pool, DEFAULT_N_FREE_CBS) < 0) {
        err_msg("[epoll_event_engine.c: ep_engine_create()] free_cb_pool_init() error");
        goto FREE_EV_Q_INIT_ERR;
    }

    return engine;

FREE_EV_Q_INIT_ERR:
    ev_cb_thread_pool_destroy(engine->thread_pool);
THREAD_POOL_CREATE_ERR:
    close(kick_evfd);
KICK_EVFD_ERR:
    close(shutdown_evfd);
SHUTDOWN_EVFD_ERR:
    close(epoll_fd);
EPOLL_CREATE_ERR:
    free(engine);
ALLOC_ENGINE_ERR:
    return NULL;
}



static EV_callback* ep_engine_get_free_cb(EP_engine* engine) {
    EV_cb_q* pool = &engine->free_cb_pool;
    EV_callback* evcb = TAILQ_FIRST(pool);
    if (evcb == NULL) {
        evcb = (EV_callback*)malloc(sizeof(*evcb));
        if (evcb == NULL) {
            err_exit_sys("[epoll_event_engine.c: ep_engine_get_free_cb()] malloc() error");
        }
    } else {
        // pop from q
        TAILQ_REMOVE(pool, evcb, cb_link);
    }
    memset(evcb, 0, sizeof(*evcb));

    return evcb;
}

static EV_callback* ep_engine_get_free_cb_locked(EP_engine* engine) {
    pthread_mutex_lock(&engine->lock);
    EV_callback* evcb = ep_engine_get_free_cb(engine);
    pthread_mutex_unlock(&engine->lock);
    return evcb;
}

int ep_engine_start_event_loop(EP_engine* engine) {
    pthread_mutex_lock(&engine->lock);
    if (engine->running_loop) {
        err_msg("[epoll_event_engine.c: ep_engine_start_event_loop()] The engine's ev loop is already running");
        pthread_mutex_unlock(&engine->lock);
        return -1;
    }
    engine->running_loop = true;
    pthread_mutex_unlock(&engine->lock);

    event_loop(engine);

    return 0;
}   


static void shutdown_event_loop(EP_engine* engine) {
    if (write_eventfd(engine->shutdown_evfd) < 0) {
        err_exit("[epoll_event_engine.c: shutdown_event_loop()] write_eventfd error");
    }
}

void ep_engine_stop_event_loop(EP_engine* engine) {
    pthread_mutex_lock(&engine->lock);
    if (!engine->running_loop) {
        pthread_mutex_unlock(&engine->lock);
        return;
    }
    if(!engine->shutting_down) {
        engine->shutting_down = true;
        shutdown_event_loop(engine);
    }
    pthread_mutex_unlock(&engine->lock);
    return;
}

static void flush_ev_callbacks(EP_engine* engine) {
    EV_callback* evcb;
    TAILQ_CONCAT(&engine->ev_callbacks.curr_list, 
                &engine->ev_callbacks.pending_list, cb_link);
    while ((evcb = TAILQ_FIRST(&engine->ev_callbacks.curr_list)) != NULL) {
        TAILQ_REMOVE(&engine->ev_callbacks.curr_list, evcb, cb_link);

        if (evcb->on_done)
            evcb->on_done(evcb->epdfd, evcb->on_done_arg);
    }
}

static void flush_destroy_fd_q(EP_engine* engine) {
    Epolled_fd* epdfd;
    TAILQ_CONCAT(&engine->destroy_fd_q.curr_list, 
                &engine->destroy_fd_q.pending_list, fd_link);
    while ((epdfd = TAILQ_FIRST(&engine->destroy_fd_q.curr_list)) != NULL) {
        TAILQ_REMOVE(&engine->destroy_fd_q.curr_list, epdfd, fd_link);

        free(epdfd);
    }
}

void ep_engine_destroy(EP_engine* engine) {
    assert(engine != NULL);

    pthread_mutex_lock(&engine->lock);
    while(engine->running_loop) {
        if(!engine->shutting_down) {
            engine->shutting_down = true;
            shutdown_event_loop(engine);
        }
        pthread_cond_wait(&engine->cv, &engine->lock);
    }
    
    if(engine->thread_pool) {
        ev_cb_thread_pool_destroy(engine->thread_pool);
    }
    pthread_mutex_unlock(&engine->lock);

    flush_ev_callbacks(engine);
    flush_destroy_fd_q(engine);

    // close fds
    close(engine->kick_evfd);
    close(engine->shutdown_evfd);
    close(engine->epoll_fd);

    free_cb_pool_destroy(&engine->free_cb_pool);
    // pthread_mutex_unlock(&engine->lock);
    pthread_mutex_destroy(&engine->lock);
    pthread_cond_destroy(&engine->cv);

    free(engine);
}

static void run_callbacks(EP_engine* engine) {
    EV_callback* cb;
    while (1) {
        pthread_mutex_lock(&engine->lock);
        TAILQ_CONCAT(&engine->ev_callbacks.curr_list, 
                    &engine->ev_callbacks.pending_list, cb_link);
        pthread_mutex_unlock(&engine->lock);

        if (TAILQ_EMPTY(&engine->ev_callbacks.curr_list))
            break;

        while ((cb = TAILQ_FIRST(&engine->ev_callbacks.curr_list)) != NULL) {
            TAILQ_REMOVE(&engine->ev_callbacks.curr_list, cb, cb_link);

            if (engine->thread_pool) {
                ev_cb_thread_pool_schedule(engine->thread_pool, cb);
            } else {
                cb->cb_fn(cb->epdfd, cb->cb_arg, cb->epdfd_state);
                //run on_done 
                if (cb->on_done)
                    cb->on_done(cb->epdfd, cb->on_done_arg);
            }
        }
    }

    return;
}

static void destroy_fds(EP_engine* engine) {
// #ifdef DEBUG_ENGINE
//     printf("[epoll_event_engine.c: destroy_fds()] Destroying fds...\n");
// #endif
    Epolled_fd* epdfd;
    while (1) {
        pthread_mutex_lock(&engine->lock);
        TAILQ_CONCAT(&engine->destroy_fd_q.curr_list, 
                    &engine->destroy_fd_q.pending_list, fd_link);
        pthread_mutex_unlock(&engine->lock);

        if (TAILQ_EMPTY(&engine->destroy_fd_q.curr_list))
            break;

        while ((epdfd = TAILQ_FIRST(&engine->destroy_fd_q.curr_list)) != NULL) {
            TAILQ_REMOVE(&engine->destroy_fd_q.curr_list, epdfd, fd_link);
            if (epdfd->on_destroy_cb)
                epdfd->on_destroy_cb(epdfd->on_destroy_arg);
        #ifdef DEBUG_ENGINE
            printf("[epoll_event_engine.c: destroy_fds()] The fd(%d) is destroyed\n", epdfd->fd);
        #endif
            
            pthread_mutex_destroy(&epdfd->lock);
            free(epdfd);
        }
    }
}

static void event_loop(EP_engine* engine) {
    if (engine->thread_pool)
        ev_cb_thread_pool_start(engine->thread_pool);

    struct epoll_event evlist[MAX_EPOLL_EVENTS];
    int nready;
    int epoll_fd = engine->epoll_fd;

    bool got_loop_shutdown = false;
    while (1) {

        if ((nready = epoll_wait(epoll_fd, evlist, MAX_EPOLL_EVENTS, -1)) < 0) {
            err_exit_sys("[epoll_event_engine.c: event_loop()] epoll_wait() error");
        }

        for (int i = 0; i < nready; i++) {
            if (evlist[i].data.fd == engine->shutdown_evfd) {
                read_eventfd(engine->shutdown_evfd);
                got_loop_shutdown = true;
                continue;
            }
            if (evlist[i].data.fd == engine->kick_evfd) {
                read_eventfd(engine->kick_evfd);
                continue;
            }

            Epolled_fd* epdfd = evlist[i].data.ptr;
            if ((evlist[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP))) {
                event_set_available(&epdfd->rev);
            }
            if ((evlist[i].events & (EPOLLOUT | EPOLLERR | EPOLLHUP))) {
                event_set_available(&epdfd->wev);
            }
        }
        
        run_callbacks(engine);
        destroy_fds(engine);
        
        if (got_loop_shutdown) {
        #ifdef DEBUG_ENGINE
            printf("[epoll_event_engine.c: event_loop()] Shutting down event loop\n");
        #endif
            break;
        }
    }

    if (engine->thread_pool) {
        ev_cb_thread_pool_stop(engine->thread_pool);
    }

    pthread_mutex_lock(&engine->lock);
    engine->running_loop = false;
    engine->shutting_down = false;
    pthread_cond_signal(&engine->cv);
    pthread_mutex_unlock(&engine->lock);
}
