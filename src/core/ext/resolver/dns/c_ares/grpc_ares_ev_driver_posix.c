/*
 *
 * Copyright 2016, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#include <grpc/support/port_platform.h>
#include "src/core/lib/iomgr/port.h"
#if !defined(GRPC_NATIVE_ADDRESS_RESOLVE) && defined(GRPC_POSIX_SOCKET)

#include "src/core/ext/resolver/dns/c_ares/grpc_ares_ev_driver.h"

#include <ares.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>
#include <grpc/support/useful.h>
#include "src/core/ext/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/lib/iomgr/ev_posix.h"
#include "src/core/lib/iomgr/iomgr_internal.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"
#include "src/core/lib/iomgr/unix_sockets_posix.h"
#include "src/core/lib/support/block_annotate.h"
#include "src/core/lib/support/string.h"

typedef struct fd_node {
  /** the owner of this fd node */
  grpc_ares_ev_driver *ev_driver;
  /** refcount of the node */
  gpr_refcount refs;
  /** the grpc_fd owned by this fd node */
  grpc_fd *grpc_fd;
  /** a closure wrapping on_readable_cb, which should be invoked when the
      grpc_fd in this node becomes readable. */
  grpc_closure read_closure;
  /** a closure wrapping on_writable_cb, which should be invoked when the
      grpc_fd in this node becomes writable. */
  grpc_closure write_closure;
  /** next fd node in the list */
  struct fd_node *next;

  /** mutex guarding the rest of the state */
  gpr_mu mu;
  /** if the readable closure has been registered */
  bool readable_registered;
  /** if the writable closure has been registered */
  bool writable_registered;
} fd_node;

struct grpc_ares_ev_driver {
  /** the ares_channel owned by this event driver */
  ares_channel channel;
  /** pollset set for driving the IO events of the channel */
  grpc_pollset_set *pollset_set;

  /** mutex guarding the rest of the state */
  gpr_mu mu;
  /** a list of grpc_fd that this event driver is currently using. */
  fd_node *fds;
  /** is this event driver currently working? */
  bool working;
};

static void grpc_ares_notify_on_event_locked(grpc_exec_ctx *exec_ctx,
                                             grpc_ares_ev_driver *ev_driver);

static fd_node *fd_node_ref(fd_node *fdn) {
  gpr_log(GPR_DEBUG, "ref %d", grpc_fd_wrapped_fd(fdn->grpc_fd));
  gpr_ref(&fdn->refs);
  return fdn;
}

static void fd_node_unref(grpc_exec_ctx *exec_ctx, fd_node *fdn) {
  gpr_log(GPR_DEBUG, "unref %d", grpc_fd_wrapped_fd(fdn->grpc_fd));
  if (gpr_unref(&fdn->refs)) {
    gpr_log(GPR_DEBUG, "delete fd: %d", grpc_fd_wrapped_fd(fdn->grpc_fd));
    GPR_ASSERT(!fdn->readable_registered);
    GPR_ASSERT(!fdn->writable_registered);
    gpr_mu_destroy(&fdn->mu);
    grpc_pollset_set_del_fd(exec_ctx, fdn->ev_driver->pollset_set,
                            fdn->grpc_fd);
    grpc_fd_shutdown(exec_ctx, fdn->grpc_fd);
    grpc_fd_orphan(exec_ctx, fdn->grpc_fd, NULL, NULL, "c-ares query finished");
    gpr_free(fdn);
  }
}

grpc_error *grpc_ares_ev_driver_create(grpc_ares_ev_driver **ev_driver,
                                       grpc_pollset_set *pollset_set) {
  int status;
  grpc_error *err = grpc_ares_init();
  if (err != GRPC_ERROR_NONE) {
    return err;
  }
  *ev_driver = gpr_malloc(sizeof(grpc_ares_ev_driver));
  status = ares_init(&(*ev_driver)->channel);
  gpr_log(GPR_DEBUG, "grpc_ares_ev_driver_create\n");
  if (status != ARES_SUCCESS) {
    char *err_msg;
    gpr_asprintf(&err_msg, "Failed to init ares channel. C-ares error: %s",
                 ares_strerror(status));
    err = GRPC_ERROR_CREATE(err_msg);
    gpr_free(err_msg);
    gpr_free(*ev_driver);
    return err;
  }
  gpr_mu_init(&(*ev_driver)->mu);
  (*ev_driver)->pollset_set = pollset_set;
  (*ev_driver)->fds = NULL;
  (*ev_driver)->working = false;
  return GRPC_ERROR_NONE;
}

static void grpc_ares_ev_driver_cleanup(grpc_exec_ctx *exec_ctx, void *arg,
                                        grpc_error *error) {
  GPR_ASSERT(error == GRPC_ERROR_NONE);
  grpc_ares_ev_driver *ev_driver = arg;
  GPR_ASSERT(ev_driver->fds == NULL);
  gpr_mu_lock(&ev_driver->mu);
  gpr_mu_unlock(&ev_driver->mu);
  gpr_mu_destroy(&ev_driver->mu);
  ares_destroy(ev_driver->channel);
  gpr_free(ev_driver);
  grpc_ares_cleanup();
}

void grpc_ares_ev_driver_destroy(grpc_exec_ctx *exec_ctx,
                                 grpc_ares_ev_driver *ev_driver) {
  // Shutdown all the working fds, invoke their registered on_readable_cb and
  // on_writable_cb.
  gpr_mu_lock(&ev_driver->mu);
  fd_node *fdn;
  for (fdn = ev_driver->fds; fdn; fdn = fdn->next) {
    grpc_fd_shutdown(exec_ctx, fdn->grpc_fd);
    fdn = fdn->next;
  }
  gpr_mu_unlock(&ev_driver->mu);
  // Schedule the actual cleanup with exec_ctx, so that it happens after the
  // fd shutdown process.
  grpc_exec_ctx_sched(
      exec_ctx, grpc_closure_create(grpc_ares_ev_driver_cleanup, ev_driver),
      GRPC_ERROR_NONE, NULL);
}

// Search fd in the fd_node list head. This is an O(n) search, the max possible
// value of n is ARES_GETSOCK_MAXNUM (16). n is typically 1 - 2 in our tests.
static fd_node *pop_fd_node(fd_node **head, int fd) {
  fd_node dummy_head;
  dummy_head.next = *head;
  fd_node *node = &dummy_head;
  while (node->next != NULL) {
    if (grpc_fd_wrapped_fd(node->next->grpc_fd) == fd) {
      fd_node *ret = node->next;
      node->next = node->next->next;
      *head = dummy_head.next;
      return ret;
    }
    node = node->next;
  }
  return NULL;
}

static void on_readable_cb(grpc_exec_ctx *exec_ctx, void *arg,
                           grpc_error *error) {
  fd_node *fdn = arg;
  grpc_ares_ev_driver *ev_driver = fdn->ev_driver;
  gpr_mu_lock(&fdn->mu);
  fdn->readable_registered = false;
  gpr_mu_unlock(&fdn->mu);

  gpr_log(GPR_DEBUG, "readable on %d", grpc_fd_wrapped_fd(fdn->grpc_fd));
  if (error == GRPC_ERROR_NONE) {
    ares_process_fd(ev_driver->channel, grpc_fd_wrapped_fd(fdn->grpc_fd),
                    ARES_SOCKET_BAD);
  } else {
    // If error is not GRPC_ERROR_NONE, it means the fd has been shutdown or
    // timed out. The pending lookups made on this ev_driver will be cancelled
    // by the following ares_cancel() and the on_done callbacks will be invoked
    // with a status of ARES_ECANCELLED. The remaining file descriptors in this
    // ev_driver will be cleaned up in the follwing
    // grpc_ares_notify_on_event_locked().
    ares_cancel(ev_driver->channel);
  }
  fd_node_unref(exec_ctx, fdn);
  gpr_mu_lock(&ev_driver->mu);
  grpc_ares_notify_on_event_locked(exec_ctx, ev_driver);
  gpr_mu_unlock(&ev_driver->mu);
}

static void on_writable_cb(grpc_exec_ctx *exec_ctx, void *arg,
                           grpc_error *error) {
  fd_node *fdn = arg;
  grpc_ares_ev_driver *ev_driver = fdn->ev_driver;
  gpr_mu_lock(&fdn->mu);
  fdn->writable_registered = false;
  gpr_mu_unlock(&fdn->mu);

  gpr_log(GPR_DEBUG, "writable on %d", grpc_fd_wrapped_fd(fdn->grpc_fd));
  if (error == GRPC_ERROR_NONE) {
    ares_process_fd(ev_driver->channel, ARES_SOCKET_BAD,
                    grpc_fd_wrapped_fd(fdn->grpc_fd));
  } else {
    // If error is not GRPC_ERROR_NONE, it means the fd has been shutdown or
    // timed out. The pending lookups made on this ev_driver will be cancelled
    // by the following ares_cancel() and the on_done callbacks will be invoked
    // with a status of ARES_ECANCELLED. The remaining file descriptors in this
    // ev_driver will be cleaned up in the follwing
    // grpc_ares_notify_on_event_locked().
    ares_cancel(ev_driver->channel);
  }
  fd_node_unref(exec_ctx, fdn);
  gpr_mu_lock(&ev_driver->mu);
  grpc_ares_notify_on_event_locked(exec_ctx, ev_driver);
  gpr_mu_unlock(&ev_driver->mu);
}

void *grpc_ares_ev_driver_get_channel(grpc_ares_ev_driver *ev_driver) {
  return &ev_driver->channel;
}

// Get the file descriptors used by the ev_driver's ares channel, register
// driver_closure with these filedescriptors.
static void grpc_ares_notify_on_event_locked(grpc_exec_ctx *exec_ctx,
                                             grpc_ares_ev_driver *ev_driver) {
  fd_node *new_list = NULL;
  ares_socket_t socks[ARES_GETSOCK_MAXNUM];
  int socks_bitmask =
      ares_getsock(ev_driver->channel, socks, ARES_GETSOCK_MAXNUM);
  for (size_t i = 0; i < ARES_GETSOCK_MAXNUM; i++) {
    if (ARES_GETSOCK_READABLE(socks_bitmask, i) ||
        ARES_GETSOCK_WRITABLE(socks_bitmask, i)) {
      fd_node *fdn = pop_fd_node(&ev_driver->fds, socks[i]);
      // Create a new fd_node if sock[i] is not in the fd_node list.
      if (fdn == NULL) {
        char *fd_name;
        gpr_asprintf(&fd_name, "ares_ev_driver-%" PRIuPTR, i);
        fdn = gpr_malloc(sizeof(fd_node));
        gpr_log(GPR_DEBUG, "new fd: %d", socks[i]);
        fdn->grpc_fd = grpc_fd_create(socks[i], fd_name);
        fdn->ev_driver = ev_driver;
        fdn->readable_registered = false;
        fdn->writable_registered = false;
        gpr_mu_init(&fdn->mu);
        gpr_ref_init(&fdn->refs, 1);
        grpc_closure_init(&fdn->read_closure, on_readable_cb, fdn);
        grpc_closure_init(&fdn->write_closure, on_writable_cb, fdn);
        grpc_pollset_set_add_fd(exec_ctx, ev_driver->pollset_set, fdn->grpc_fd);
        gpr_free(fd_name);
      }
      fdn->next = new_list;
      new_list = fdn;
      gpr_mu_lock(&fdn->mu);
      // Register read_closure if the socket is readable and read_closure has
      // not been registered with this socket.
      if (ARES_GETSOCK_READABLE(socks_bitmask, i) &&
          !fdn->readable_registered) {
        fd_node_ref(fdn);
        gpr_log(GPR_DEBUG, "notify read on: %d",
                grpc_fd_wrapped_fd(fdn->grpc_fd));
        grpc_fd_notify_on_read(exec_ctx, fdn->grpc_fd, &fdn->read_closure);
        fdn->readable_registered = true;
      }
      // Register write_closure if the socket is writable and write_closure has
      // not been registered with this socket.
      if (ARES_GETSOCK_WRITABLE(socks_bitmask, i) &&
          !fdn->writable_registered) {
        gpr_log(GPR_DEBUG, "notify write on: %d",
                grpc_fd_wrapped_fd(fdn->grpc_fd));
        fd_node_ref(fdn);
        grpc_fd_notify_on_write(exec_ctx, fdn->grpc_fd, &fdn->write_closure);
        fdn->writable_registered = true;
      }
      gpr_mu_unlock(&fdn->mu);
    }
  }
  // Any remaining fds in ev_driver->fds was not returned by ares_getsock() and
  // is therefore no longer in use, so they can be shut donw and removed from
  // the list.
  while (ev_driver->fds != NULL) {
    fd_node *cur = ev_driver->fds;
    ev_driver->fds = ev_driver->fds->next;
    grpc_fd_shutdown(exec_ctx, cur->grpc_fd);
    fd_node_unref(exec_ctx, cur);
  }
  ev_driver->fds = new_list;
  // If the ev driver has no working fd, all the tasks are done.
  if (new_list == NULL) {
    ev_driver->working = false;
    gpr_log(GPR_DEBUG, "ev driver stop working");
  }
}

void grpc_ares_ev_driver_start(grpc_exec_ctx *exec_ctx,
                               grpc_ares_ev_driver *ev_driver) {
  gpr_mu_lock(&ev_driver->mu);
  if (!ev_driver->working) {
    ev_driver->working = true;
    grpc_ares_notify_on_event_locked(exec_ctx, ev_driver);
  }
  gpr_mu_unlock(&ev_driver->mu);
}

#endif /* !GRPC_NATIVE_ADDRESS_RESOLVE && GRPC_POSIX_SOCKET */
