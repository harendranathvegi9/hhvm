/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include <sys/mman.h>

#include <runtime/base/timeout_thread.h>
#include <runtime/base/runtime_option.h>
#include <util/lock.h>
#include <util/logger.h>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////
// statics

// class defined in runtime/base/types.h
static void on_timer(int fd, short events, void *context) {
  ((TimeoutThread*)context)->onTimer(fd);
}

static void on_thread_stop(int fd, short events, void *context) {
  event_base_loopbreak((struct event_base *)context);
}

///////////////////////////////////////////////////////////////////////////////

void TimeoutThread::DeferTimeout(int seconds) {
  RequestInjectionData &data = ThreadInfo::s_threadInfo->m_reqInjectionData;
  if (seconds > 0) {
    // cheating by resetting started to desired timestamp
    data.started = time(0) + (seconds - data.timeoutSeconds);
  } else {
    data.started = 0;
  }
}

///////////////////////////////////////////////////////////////////////////////

TimeoutThread::TimeoutThread(int timerCount, int timeoutSeconds)
  : m_numWorkers(0), m_numTimers(0), m_stopped(false),
    m_timeoutSeconds(timeoutSeconds) {
  ASSERT(timerCount > 0);

  m_eventBase = event_base_new();
  m_eventTimeouts.resize(timerCount);
  m_timeoutData.resize(timerCount);

  // We need to open the pipe here because worker threads can start
  // before the timeout thread starts
  m_pipe.open();
}

TimeoutThread::~TimeoutThread() {
  event_base_free(m_eventBase);
}

void TimeoutThread::registerRequestThread(RequestInjectionData* data) {
  ASSERT(data);
  data->timeoutSeconds = m_timeoutSeconds;

  // Add the new worker to the timeout thread's list of workers
  {
    Lock lock(this);
    ASSERT(m_numWorkers < (int)m_timeoutData.size());
    m_timeoutData[m_numWorkers++] = data;
  }

  // Write to the timeout thread's pipe so that it wakes up and
  // creates a timer for the new worker
  if (write(m_pipe.getIn(), "", 1) < 0) {
    Logger::Warning("Error notifying the timeout thread that a new "
                    "worker has started");
  }
}

void TimeoutThread::checkForNewWorkers() {
  // If m_timeoutSeconds is not a positive number, then workers threads
  // are allowed to run forever, so don't bother creating timers for the
  // workers
  if (m_timeoutSeconds <= 0) {
    return;
  }
  Lock lock(this);
  // If there are new workers, create timers for them
  if (m_numWorkers > m_numTimers) {
    struct timeval timeout;
    timeout.tv_usec = 0;
    // +2 to make sure when it times out, this equation always holds:
    //   time(0) - RequestInjection::s_reqInjectionData->started >=
    //     m_timeoutSeconds
    timeout.tv_sec = m_timeoutSeconds + 2;
    for (int i = m_numTimers; i < m_numWorkers; ++i) {
      event *e = &m_eventTimeouts[i];
      event_set(e, i, 0, on_timer, this);
      event_base_set(m_eventBase, e);
      event_add(e, &timeout);
    }
    m_numTimers = m_numWorkers;
  }
}

void TimeoutThread::drainPipe() {
  struct pollfd fdArray[1];
  fdArray[0].fd = m_pipe.getOut();
  fdArray[0].events = POLLIN;
  while (poll(fdArray, 1, 0) > 0) {
    char buf[256];
    read(m_pipe.getOut(), buf, 256);
  }
}

void TimeoutThread::run() {
  event_set(&m_eventPipe, m_pipe.getOut(), EV_READ|EV_PERSIST,
            on_thread_stop, m_eventBase);
  event_base_set(m_eventBase, &m_eventPipe);
  event_add(&m_eventPipe, NULL);

  while (!m_stopped) {
    checkForNewWorkers();
    event_base_loop(m_eventBase, EVLOOP_ONCE);
    drainPipe();
  }

  for (int i = 0; i < m_numTimers; ++i) {
    event_del(&m_eventTimeouts[i]);
  }
  event_del(&m_eventPipe);
}

void TimeoutThread::stop() {
  m_stopped = true;
  if (write(m_pipe.getIn(), "", 1) < 0) {
    // an error occured but we're in shutdown already, so ignore
  }
}

void TimeoutThread::onTimer(int index) {
  ASSERT(index >= 0 && index < (int)m_eventTimeouts.size());

  event *e = &m_eventTimeouts[index];
  event_del(e);

  RequestInjectionData *data = m_timeoutData[index];
  ASSERT(data);
  struct timeval timeout;
  timeout.tv_usec = 0;
  if (data->started > 0) {
    time_t now = time(0);
    int delta = now - data->started;
    if (delta >= m_timeoutSeconds) {
      timeout.tv_sec = m_timeoutSeconds + 2;
      if (hhvm) {
        Lock l(data->surpriseLock);
        data->setTimedOutFlag();
        if (data->surprisePage) {
          mprotect(data->surprisePage, sizeof(void*), PROT_NONE);
        }
      } else {
        data->setTimedOutFlag();
      }
    } else {
      // Negative delta means start time was adjusted forward to give more time
      if (delta < 0) delta = 0;

      // otherwise, a new request started after we started the timer
      timeout.tv_sec = m_timeoutSeconds - delta + 2;
    }
  } else {
    // Another cycle of m_timeoutSeconds
    timeout.tv_sec = m_timeoutSeconds;
  }

  event_set(e, index, 0, on_timer, this);
  event_base_set(m_eventBase, e);
  event_add(e, &timeout);
}

///////////////////////////////////////////////////////////////////////////////
}
