#include "event_queue.hpp"
#include <string.h>
#include "arch/linux/thread_pool.hpp"
#include "concurrency/cond_var.hpp"

// TODO: I guess signum is unused because we aren't interested, and
// uctx is unused because we don't have any context data.  Is this the
// true?
void event_queue_base_t::signal_handler(UNUSED int signum, siginfo_t *siginfo, UNUSED void *uctx) {
    linux_event_callback_t *callback = (linux_event_callback_t*)siginfo->si_value.sival_ptr;
    callback->on_event(siginfo->si_overrun);
}

// TODO: Why is cb unused?
void event_queue_base_t::watch_signal(const sigevent *evp, UNUSED linux_event_callback_t *cb) {
    // All events are automagically blocked by thread pool, this is a
    // typical use case for epoll_pwait/ppoll.
    
    // Establish a handler on the signal that calls the right callback
    struct sigaction sa;
    bzero((char*)&sa, sizeof(struct sigaction));
    sa.sa_sigaction = &event_queue_base_t::signal_handler;
    sa.sa_flags = SA_SIGINFO;
    
    int res = sigaction(evp->sigev_signo, &sa, NULL);
    guarantee_err(res == 0, "Could not install signal handler in event queue");
}

// TODO: Why is cb unused?
void event_queue_base_t::forget_signal(UNUSED const sigevent *evp, UNUSED linux_event_callback_t *cb) {
    // We don't support forgetting signals for now
}

/* linux_event_watcher_t */

struct linux_event_watcher_guts_t : public linux_event_callback_t {

    linux_event_watcher_guts_t(fd_t fd, linux_event_callback_t *eh) :
        fd(fd), error_handler(eh),
        read_handler(this), write_handler(this),
        old_mask(0), registration_thread(-1)
        { }

    ~linux_event_watcher_guts_t() {
        rassert(!read_handler.mc);
        rassert(!write_handler.mc);
    }

    fd_t fd;   // the file descriptor to watch

    linux_event_callback_t *error_handler;   // What to call if there is an error

    /* These objects handle waiting for reads and writes. Mostly they exist to be subclasses
    of multicond_t::waiter_t. */
    struct waiter_t : public multicond_t::waiter_t {
    
        waiter_t(linux_event_watcher_guts_t *p) : parent(p), mc(NULL) { }
    
        linux_event_watcher_guts_t *parent;
        multicond_t *mc;
    
        void watch(multicond_t *m) {
            rassert(!mc);
            mc = m;
            mc->add_waiter(this);
            parent->remask();
        }
        void pulse() {
            rassert(mc);
            mc->pulse();   // Calls on_multicond_pulsed()
        }
        void on_multicond_pulsed() {
            rassert(mc);
            mc = NULL;
            parent->remask();
        }
    };
    waiter_t read_handler, write_handler;

    int old_mask;   // So remask() knows whether we were registered with the event queue before

    int registration_thread;   // The event queue we are registered with, or -1 if we are not registered

    /* If the callback for some event causes the linux_event_watcher_t to be destroyed,
    these variables will ensure that the linux_event_watcher_guts_t doesn't get destroyed
    immediately. */
    bool dont_destroy_yet, should_destroy;

    void on_event(int event) {

        dont_destroy_yet = true;

        int error_mask = poll_event_err | poll_event_hup | poll_event_rdhup;
        if (event & error_mask) {
            error_handler->on_event(event & error_mask);
        }

        if (event & poll_event_in) {
            read_handler.pulse();
        }

        if (event & poll_event_out) {
            write_handler.pulse();
        }

        dont_destroy_yet = false;
        if (should_destroy) delete this;
    }

    void watch(int event, multicond_t *mc) {
        rassert(event == poll_event_in || event == poll_event_out);
        waiter_t *handler = event == poll_event_in ? &read_handler : &write_handler;
        handler->watch(mc);
    }

    void remask() {
        /* Change our registration with the event queue depending on what events
        we are actually waiting for. */

        int new_mask = 0;
        if (read_handler.mc) new_mask |= poll_event_in;
        if (write_handler.mc) new_mask |= poll_event_out;

        if (old_mask) {
            rassert(registration_thread == linux_thread_pool_t::thread_id);
            if (new_mask == 0) {
                linux_thread_pool_t::thread->queue.forget_resource(fd, this);
                registration_thread = -1;
            } else if (new_mask != old_mask) {
                linux_thread_pool_t::thread->queue.adjust_resource(fd, new_mask, this);
            }
        } else {
            rassert(registration_thread == -1);
            if (new_mask == 0) {
                /* We went from not watching any events to not watching any events. */
            } else {
                linux_thread_pool_t::thread->queue.watch_resource(fd, new_mask, this);
                registration_thread = linux_thread_pool_t::thread_id;
            }
        }

        old_mask = new_mask;
    }
};

linux_event_watcher_t::linux_event_watcher_t(fd_t f, linux_event_callback_t *eh)
    : guts(new linux_event_watcher_guts_t(f, eh)) { }

linux_event_watcher_t::~linux_event_watcher_t() {
    if (guts->dont_destroy_yet) {
        guts->should_destroy = true;
    } else {
        delete guts;
    }
}

void linux_event_watcher_t::watch(int event, multicond_t *to_signal) {
    guts->watch(event, to_signal);
}