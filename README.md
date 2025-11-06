# ep_engine — epoll-based Event Engine

<!-- ![Event Loop Workflow](images/ep_engine_loop.png) -->

## Overview
**ep_engine** is a lightweight **event-driven engine** built upon Linux’s `epoll` interface. 

---

## Features
* This project is developed with reference to the designs of Google’s gRPC, libevent, and nginx.
* This is an **epoll-based event engine**.
* Works in **edge-triggered mode**.
* Applications using this engine must follow the recommended approach for edge-triggered mode described in the `epoll(7)` man page:
    1. Use **non-blocking file descriptors**.  
       (For this engine, set nonblocking mode before creating an `Epolled_fd`.)
    2. Wait for an event **only after** `read(2)` or `write(2)` return `EAGAIN`.  
       (For this engine, inside an event callback, continuously read/write the fd until `EAGAIN`,  
       and then (re)register the handler callback to get event notifications again using  
       `epdfd_notify_on_read()` or `epdfd_notify_on_writable()`.)
* This project is mainly for personal use, not for production codes.\
  Thus, the code may lack thorough testing, so please use it with caution.
* This project uses my [utils](https://github.com/wjnlim/utils.git) library. The CMake file will automatically fetch the utils project internally
---

## Core Concepts

### `Epolled_fd`

Represents a file descriptor being polled by the **epoll engine (ep_engine)**.

Each `Epolled_fd` internally maintains **read/write event objects**, each of which has its own state.

---

## Event States

| State | Description |
|-------|--------------|
| **EV_HANDLER_ARMED** | An event handler is armed to be scheduled when an I/O event (readable/writable) occurs. |
| **EV_NOT_AVAILABLE** | No available I/O event (no event has occurred, or it’s already handled/scheduled). |
| **EV_AVAILABLE** | An I/O event is ready to be handled/scheduled. |
| **EV_CLOSED** | Not interested in the event anymore. |

---

## APIs

### `epdfd_notify_on_read()` / `epdfd_notify_on_writable()`

Registers a read/write event callback.  
The callback will be called when the `Epolled_fd` becomes ready (or is already ready) to read or write.

**Usage Notes:**
- To get event notifications again after the callback is called, this must be called again inside the callback **after handling** the event (e.g., after `read()`/`write()` return `EAGAIN`).

**State Transitions:**\
EV_NOT_AVAILABLE -> EV_HANDLER_ARMED: handler registered\
EV_AVAILABLE -> EV_NOT_AVAILABLE: if I/O event exists, schedule callback immediately

---

### `epdfd_set_read()` / `epdfd_set_writable()`

Sets the `Epolled_fd` as ready to read/write and schedules the registered callback.

**State Transitions:**\
EV_HANDLER_ARMED -> EV_NOT_AVAILABLE: schedule registered callback\
EV_NOT_AVAILABLE -> EV_AVAILABLE: if the fd is not re-armed yet, mark event as 'has an I/O event' to let the `epdfd_notify_on_read/write()` handle it correctly.

---

## Event Loop Workflow

The following diagram illustrates how the **ep_engine's event loop** operates:

1. Waits for events via `epoll_wait()`.
2. When file descriptors become readable/writable (or the loop is kicked),  
   it marks events as ready.
3. Depending on configuration, event callbacks are either:
   - Run directly in the loop, or  
   - Dispatched to a **thread pool** for concurrent execution.

![Event Loop Workflow](images/ep_engine_loop.png)