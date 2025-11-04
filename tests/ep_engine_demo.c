#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "ep_engine/epoll_event_engine.h"

static void read_cb(Epolled_fd* epdfd, void* arg, Epdfd_state state) {
    printf("Read event (edge) triggered!\n");
    char buf[256];
    ssize_t nread;
    while ((nread = read(epdfd_get_fd(epdfd), buf, sizeof(buf) - 1)) > 0) {
        buf[nread] = '\0';
        printf("You typed: %s", buf);
    }
    if (nread < 0 && errno == EAGAIN) {
        printf("Register read event callback again!\n");
        epdfd_notify_on_readable(epdfd, read_cb, NULL);
    } else {
        printf("EOF or read error, exiting loop.\n");
        ep_engine_stop_event_loop(epdfd_get_engine(epdfd));
    }
}

int main(void) {
    EP_engine* engine = ep_engine_create(true, 1);

    // setnonblock 
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) != 0) {
        printf("[ep_engine_test.c:main()] fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) error\n");
    }
    Epolled_fd* epd_stdin = epdfd_create(STDIN_FILENO, engine);
    epdfd_notify_on_readable(epd_stdin, read_cb, NULL);

    ep_engine_start_event_loop(engine);
    printf("Event loop exited.\n");

    epdfd_destroy(epd_stdin, NULL, NULL);
    ep_engine_destroy(engine);

    return EXIT_SUCCESS;
}