#ifndef EV_CB_THREAD_POOL_H
#define EV_CB_THREAD_POOL_H

#include "ev_callback.h"

typedef struct EV_cb_thread_pool EV_cb_thread_pool;

EV_cb_thread_pool* ev_cb_thread_pool_create(int num_threads);
void ev_cb_thread_pool_start(EV_cb_thread_pool* tp);
void ev_cb_thread_pool_stop(EV_cb_thread_pool* tp);
void ev_cb_thread_pool_schedule(EV_cb_thread_pool* tp, EV_callback* cb);
void ev_cb_thread_pool_destroy(EV_cb_thread_pool* tp);

#endif