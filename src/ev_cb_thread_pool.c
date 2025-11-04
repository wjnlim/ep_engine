#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>
#include <stdbool.h>

#include "ev_cb_thread_pool.h"
#include "utils/my_err.h"
// #include "ev_cb_thread_pool.h"
#include "utils_ds/blocking_queue.h"
#include "ev_callback.h"
// #include "ev_cb_thread_pool.h"
// #define WORKQUEUE_SIZE 128

struct EV_cb_thread_pool {
    Blocking_Q* bq;
    int num_threads;
    pthread_t* worker_threads;
    bool running_workers;
    EV_callback* terminate_thread_cbs; //for shutting down worker threads
};

// static void terminate_thread(void* arg) {
static void terminate_thread(Epolled_fd* epdfd, void* arg, Epdfd_state state) {
    pthread_exit(NULL);
}

EV_cb_thread_pool* ev_cb_thread_pool_create(int num_threads) {

    EV_cb_thread_pool* tp = (EV_cb_thread_pool*)malloc(sizeof(*tp));
    if (tp == NULL) {
        err_msg_sys("[ev_cb_thread_pool.c: ev_cb_thread_pool_create()] malloc(tp) error");
        goto TP_ALLOC_ERR;
    }

    Blocking_Q* bq = bq_create();
    if (bq == NULL) {
        err_msg("[ev_cb_thread_pool.c: ev_cb_thread_pool_create()] bq_create() error");
        goto BQ_ALLOC_ERR;
    }
    tp->bq = bq;

    tp->num_threads = num_threads;
    pthread_t* worker_threads = malloc(num_threads * sizeof(*worker_threads));
    if (worker_threads == NULL) {
        err_msg_sys("[ev_cb_thread_pool.c: ev_cb_thread_pool_create()] malloc(worker_threads) error");
        goto THREAD_ARR_ALLOC_ERR;
    }
    tp->worker_threads = worker_threads;
    tp->running_workers = false;

    EV_callback* terminate_thread_cbs = malloc(num_threads * sizeof(*terminate_thread_cbs));
    if (terminate_thread_cbs == NULL) {
        err_msg_sys("[ev_cb_thread_pool.c: ev_cb_thread_pool_create()] malloc(null cbs) error");
        goto TERM_CBS_ALLOC_ERR;
    }
    tp->terminate_thread_cbs = terminate_thread_cbs;

    for (int i = 0; i < num_threads; i++) {
        tp->terminate_thread_cbs[i].cb_fn = terminate_thread;
        tp->terminate_thread_cbs[i].cb_arg = NULL;
    }

    return tp;

TERM_CBS_ALLOC_ERR:
    free(worker_threads);
THREAD_ARR_ALLOC_ERR:
    bq_destroy(bq);
BQ_ALLOC_ERR:
    free(tp);
TP_ALLOC_ERR:
    return NULL;
}

static void* ev_tp_worker_run(void* arg) {
    EV_cb_thread_pool* tp = arg;

    Blocking_Q* bq = tp->bq;
    EV_callback* cb;
    while (1) {
        cb = bq_pop(bq);
        if (cb->cb_fn == NULL) {
            err_exit("[ev_cb_thread_pool.c: tp_worker_run()] Cannot run NULL cb func.");
        }
#ifdef DEBUG_ENGINE
        // printf("[ev_cb_thread_pool.c: ev_tp_worker_run()] Execute cb(%s) at thread %lu\n", cb->name, pthread_self());
        printf("[ev_cb_thread_pool.c: ev_tp_worker_run()] Execute cb(%p) at thread %lu\n", cb->cb_fn, pthread_self());
#endif

        cb->cb_fn(cb->epdfd, cb->cb_arg, cb->epdfd_state);

        if (cb->on_done) {
            cb->on_done(cb->epdfd, cb->on_done_arg);
        }
    }

    return NULL;
}

void ev_cb_thread_pool_start(EV_cb_thread_pool* tp) {
    tp->running_workers = true;
    int err;
    for (int i = 0; i < tp->num_threads; i++) {
        err = pthread_create(&tp->worker_threads[i], NULL, ev_tp_worker_run, (void*)tp);
        if (err) {
            err_exit_errn(err, "[ev_cb_thread_pool.c: ev_cb_thread_pool_start()] pthread_create() error");
        }
    }
}

void ev_cb_thread_pool_stop(EV_cb_thread_pool* tp) {
    tp->running_workers = false;
    for (int i = 0; i < tp->num_threads; i++) {
        bq_push(tp->bq, &tp->terminate_thread_cbs[i]);
    }

    for (int i = 0; i < tp->num_threads; i++) {
        pthread_join(tp->worker_threads[i], NULL);
    }
}

void ev_cb_thread_pool_schedule(EV_cb_thread_pool* tp, EV_callback* cb) {
    assert(tp != NULL && cb != NULL);
#ifdef DEBUG_ENGINE
    // printf("[ev_cb_thread_pool.c: ev_cb_thread_pool_schedule()] Scheduling cb(%s)\n", cb->name);
    printf("[ev_cb_thread_pool.c: ev_cb_thread_pool_schedule()] Scheduling cb(%p)\n", cb->cb_fn);
#endif
    bq_push(tp->bq, cb);
}

void ev_cb_thread_pool_destroy(EV_cb_thread_pool* tp) {
    assert(tp != NULL);
#ifdef DEBUG_ENGINE
    printf("[ev_cb_thread_pool.c: ev_cb_thread_pool_destroy()] Destroying thread pool...\n");
#endif
    if (tp->running_workers) {
        ev_cb_thread_pool_stop(tp);
    }

    bq_destroy(tp->bq);
    free(tp->terminate_thread_cbs);
    free(tp->worker_threads);
    free(tp);
}
