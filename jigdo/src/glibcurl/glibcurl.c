/* $Id$ -*- C -*-
  __   _
  |_) /|  Copyright (C) 2004  |  richard@
  | \/�|  Richard Atterer     |  atterer.net
  � '` �
  All rights reserved.

  Permission to use, copy, modify, and distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
  DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
  OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
  USE OR OTHER DEALINGS IN THE SOFTWARE.

  Except as contained in this notice, the name of a copyright holder shall
  not be used in advertising or otherwise to promote the sale, use or other
  dealings in this Software without prior written authorization of the
  copyright holder.

*/

/* #include <config.h> */

#include <glib.h>
#include <glibcurl.h>

#include <stdio.h>
#include <string.h>
#include <assert.h>
/*______________________________________________________________________*/

/* Number of highest allowed fd */
#define GLIBCURL_FDMAX 127

/* Timeout for the fds passed to glib's poll() call, in millisecs.
   curl_multi_fdset(3) says we should call curl_multi_perform() at regular
   intervals. */
#define GLIBCURL_TIMEOUT 1000

/* #define D(_args) fprintf _args; */
#define D(_args)

/* GIOCondition event masks */
#define GLIBCURL_READ  (G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP)
#define GLIBCURL_WRITE (G_IO_OUT | G_IO_ERR | G_IO_HUP)
#define GLIBCURL_EXC   (G_IO_ERR | G_IO_HUP)

/* A structure which "derives" (in glib speak) from GSource */
typedef struct CurlGSource_ {
  GSource source; /* First: The type we're deriving from */

  CURLM* multiHandle;

  /* Previously seen FDs, for comparing with libcurl's current fd_sets */
  GPollFD lastPollFd[GLIBCURL_FDMAX + 1];
  int lastPollFdMax; /* Index of highest non-empty entry in lastPollFd */

  int callPerform; /* Non-zero => curl_multi_perform() gets called */

  /* For data returned by curl_multi_fdset */
  fd_set fdRead;
  fd_set fdWrite;
  fd_set fdExc;
  int fdMax;

} CurlGSource;

/* Global state: Our CurlGSource object */
static CurlGSource* curlSrc = 0;

/* The "methods" of CurlGSource */
static gboolean prepare(GSource* source, gint* timeout);
static gboolean check(GSource* source);
static gboolean dispatch(GSource* source, GSourceFunc callback,
                         gpointer user_data);
static void finalize(GSource* source);

static GSourceFuncs curlFuncs = {
  &prepare, &check, &dispatch, &finalize, 0, 0
};
/*______________________________________________________________________*/

void glibcurl_init() {
  int fd;
  /* Create source object for curl file descriptors, and hook it into the
     default main context. */
  curlSrc = (CurlGSource*)g_source_new(&curlFuncs, sizeof(CurlGSource));
  g_source_attach(&curlSrc->source, NULL);

  /* Init rest of our data */
  memset(&curlSrc->lastPollFd, 0, sizeof(curlSrc->lastPollFd));
  for (fd = 1; fd <= GLIBCURL_FDMAX; ++fd)
    curlSrc->lastPollFd[fd].fd = fd;
  curlSrc->lastPollFdMax = 0;
  curlSrc->callPerform = 0;

  /* Init libcurl */
  curl_global_init(CURL_GLOBAL_ALL);
  curlSrc->multiHandle = curl_multi_init();

  D((stderr, "events: R=%x W=%x X=%x\n", GLIBCURL_READ, GLIBCURL_WRITE,
     GLIBCURL_EXC));
}
/*______________________________________________________________________*/

CURLM* glibcurl_handle() {
  return curlSrc->multiHandle;
}
/*______________________________________________________________________*/

CURLMcode glibcurl_add(CURL *easy_handle) {
  assert(curlSrc->multiHandle != 0);
  curlSrc->callPerform = -1;
  return curl_multi_add_handle(curlSrc->multiHandle, easy_handle);
}
/*______________________________________________________________________*/

CURLMcode glibcurl_remove(CURL *easy_handle) {
  assert(curlSrc != 0);
  assert(curlSrc->multiHandle != 0);
  return curl_multi_remove_handle(curlSrc->multiHandle, easy_handle);
}
/*______________________________________________________________________*/

/* Call this whenever you have added a request using curl_multi_add_handle().
   This is necessary to start new requests. It does so by triggering a call
   to curl_multi_perform() even in the case where no open fds cause that
   function to be called anyway. */
void glibcurl_start() {
  curlSrc->callPerform = -1;
}
/*______________________________________________________________________*/

void glibcurl_set_callback(GlibcurlCallback function, void* data) {
  g_source_set_callback(&curlSrc->source, (GSourceFunc)function, data,
                        NULL);
}
/*______________________________________________________________________*/

void glibcurl_cleanup() {
  /* You must call curl_multi_remove_handle() and curl_easy_cleanup() for all
     requests before calling this. */
/*   assert(curlSrc->callPerform == 0); */

  curl_multi_cleanup(curlSrc->multiHandle);
  curlSrc->multiHandle = 0;
  curl_global_cleanup();

/*   g_source_destroy(&curlSrc->source); */
  g_source_unref(&curlSrc->source);
  curlSrc = 0;
}
/*______________________________________________________________________*/

#ifdef G_OS_WIN32
static void registerUnregisterFds() {
#warning mingw32-runtime
  return;
}

#else

static void registerUnregisterFds() {
  int fd, fdMax;

  FD_ZERO(&curlSrc->fdRead);
  FD_ZERO(&curlSrc->fdWrite);
  FD_ZERO(&curlSrc->fdExc);
  curlSrc->fdMax = -1;
  /* What fds does libcurl want us to poll? */
  curl_multi_fdset(curlSrc->multiHandle, &curlSrc->fdRead,
                   &curlSrc->fdWrite, &curlSrc->fdExc, &curlSrc->fdMax);
  /*fprintf(stderr, "registerUnregisterFds: fdMax=%d\n", curlSrc->fdMax);*/
  assert(curlSrc->fdMax >= -1 && curlSrc->fdMax <= GLIBCURL_FDMAX);

  fdMax = curlSrc->fdMax;
  if (fdMax < curlSrc->lastPollFdMax) fdMax = curlSrc->lastPollFdMax;

  /* Has the list of required events for any of the fds changed? */
  for (fd = 0; fd <= fdMax; ++fd) {
    gushort events = 0;
    if (FD_ISSET(fd, &curlSrc->fdRead))  events |= GLIBCURL_READ;
    if (FD_ISSET(fd, &curlSrc->fdWrite)) events |= GLIBCURL_WRITE;
    if (FD_ISSET(fd, &curlSrc->fdExc))   events |= GLIBCURL_EXC;

    /* List of events unchanged => no (de)registering */
    if (events == curlSrc->lastPollFd[fd].events) continue;

    D((stderr, "registerUnregisterFds: fd %d: old events %x, "
       "new events %x\n", fd, curlSrc->lastPollFd[fd].events, events));

    /* fd is already a lastPollFd, but event type has changed => do nothing.
       Due to the implementation of g_main_context_query(), the new event
       flags will be picked up automatically. */
    if (events != 0 && curlSrc->lastPollFd[fd].events != 0) {
      curlSrc->lastPollFd[fd].events = events;
      continue;
    }
    curlSrc->lastPollFd[fd].events = events;

    /* Otherwise, (de)register as appropriate */
    if (events == 0) {
      g_source_remove_poll(&curlSrc->source, &curlSrc->lastPollFd[fd]);
      curlSrc->lastPollFd[fd].revents = 0;
      D((stderr, "unregister fd %d\n", fd));
    } else {
      g_source_add_poll(&curlSrc->source, &curlSrc->lastPollFd[fd]);
      D((stderr, "register fd %d\n", fd));
    }
  }

  curlSrc->lastPollFdMax = curlSrc->fdMax;
}
#endif

/* Called before all the file descriptors are polled by the glib main loop.
   We must have a look at all fds that libcurl wants polled. If any of them
   are new/no longer needed, we have to (de)register them with glib. */
gboolean prepare(GSource* source, gint* timeout) {
  assert(source == &curlSrc->source);

  if (curlSrc->multiHandle == 0) return FALSE;

  registerUnregisterFds();

/*   D((stderr, "prepare\n")); */
  *timeout = GLIBCURL_TIMEOUT;
/*   return FALSE; */
  return curlSrc->callPerform == -1 ? TRUE : FALSE;
}
/*______________________________________________________________________*/

/* Called after all the file descriptors are polled by glib.
   g_main_context_check() has copied back the revents fields (set by glib's
   poll() call) to our GPollFD objects. How inefficient all that copying
   is... let's add some more and copy the results of these revents into
   libcurl's fd_sets! */
gboolean check(GSource* source) {
  int fd, somethingHappened = 0;

  if (curlSrc->multiHandle == 0) return FALSE;

  assert(source == &curlSrc->source);
  FD_ZERO(&curlSrc->fdRead);
  FD_ZERO(&curlSrc->fdWrite);
  FD_ZERO(&curlSrc->fdExc);
  for (fd = 0; fd <= curlSrc->fdMax; ++fd) {
    gushort revents = curlSrc->lastPollFd[fd].revents;
    if (revents == 0) continue;
    somethingHappened = 1;
/*     D((stderr, "[fd%d] ", fd)); */
    if (revents & (G_IO_IN | G_IO_PRI))
      FD_SET((unsigned)fd, &curlSrc->fdRead);
    if (revents & G_IO_OUT)
      FD_SET((unsigned)fd, &curlSrc->fdWrite);
    if (revents & (G_IO_ERR | G_IO_HUP))
      FD_SET((unsigned)fd, &curlSrc->fdExc);
  }
/*   D((stderr, "check: fdMax %d\n", curlSrc->fdMax)); */

/*   return TRUE; */
/*   return FALSE; */
  return curlSrc->callPerform == -1 || somethingHappened != 0 ? TRUE : FALSE;
}
/*______________________________________________________________________*/

gboolean dispatch(GSource* source, GSourceFunc callback,
                  gpointer user_data) {
  CURLMcode x;

  assert(source == &curlSrc->source);
  assert(curlSrc->multiHandle != 0);
  do {
    x = curl_multi_perform(curlSrc->multiHandle, &curlSrc->callPerform);
/*     D((stderr, "dispatched %d\n", x)); */
  } while (x == CURLM_CALL_MULTI_PERFORM);

  /* If no more calls to curl_multi_perform(), unregister left-over fds */
  if (curlSrc->callPerform == 0) registerUnregisterFds();

  if (callback != 0) (*callback)(user_data);

  return TRUE; /* "Do not destroy me" */
}
/*______________________________________________________________________*/

void finalize(GSource* source) {
  assert(source == &curlSrc->source);
  registerUnregisterFds();
}
/*______________________________________________________________________*/

void glibcurl_add_proxy(const gchar *protocol, const gchar *proxy) { }
void glibcurl_add_noproxy(const gchar *host) { }