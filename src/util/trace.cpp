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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string>

/*
 * Forcibly define USE_TRACE, so we get the debug trace.h interface included
 * here. This allows mixed compilation, where some units were compiled
 * DEBUG and others compiled RELEASE, to successfully link.
 */
#ifndef USE_TRACE
#  define USE_TRACE 1
#endif
#include "runtime/base/complex_types.h"
#include "runtime/vm/core_types.h"
#include "trace.h"
#include "ringbuffer.h"

namespace HPHP {

static const Trace::Module TRACEMOD = Trace::tprefix;

using std::string;
using namespace HPHP::VM;
string tname(DataType t) {
  switch(t) {
#define CS(name) \
    case KindOf ## name: return string(#name);
    CS(Uninit)
    CS(Null)
    CS(Boolean)
    CS(Int32)
    CS(Int64)
    CS(Double)
    CS(StaticString)
    CS(String)
    CS(Array)
    CS(Object)
    CS(Variant)
    CS(Class)
#undef CS
    case KindOfInvalid: return string("Invalid");

    default: {
      char buf[128];
      sprintf(buf, "Unknown:%d", t);
      return string(buf);
    }
  }
}

string TypedValue::pretty() const  {
  char buf[20];
  sprintf(buf, "0x%lx", long(m_data.num));
  return Trace::prettyNode(tname(m_type).c_str(), string(buf));
}

namespace Trace {

int levels[NumModules];
static FILE* out;

const char *tokNames[] = {
#define TM(x) #x,
  TRACE_MODULES
#undef TM
};

/*
 * Dummy class to get some code to run before main().
 */
class Init {
  Module name2mod(const char *name) {
    for (int i = 0; i < NumModules; i++) {
      if (!strcmp(tokNames[i], name)) {
        return (Module)i;
      }
    }
    return (Module)-1;
  }

  public:
  Init() {
    /* Parse the environment for flags. */
    const char *envName = "TRACE";
    const char *env = getenv(envName);
    const char *file = getenv("HPHP_TRACE_FILE");
    if (!file) file = "/tmp/hphp.log";
    if (env) {
      out = fopen(file, "w");
      if (!out) {
        fprintf(stderr, "could not create log file (%s); using stderr\n", file);
        out = stderr;
      }
      char *e = strdup(env);
      char *tok;
      for (tok = strtok(e, ","); tok; tok = strtok(NULL, ",")) {
        char *ctok;
        char *moduleName = tok;
        if (( ctok = strchr(moduleName, ':'))) {
          *ctok++ = 0;
        }
        int level = ctok ? atoi(ctok) : 1;
        int mod = name2mod(moduleName);
        if (mod >= 0) {
          levels[mod] = level;
        }
      }
      free(e);
    } else {
      // If TRACE env var is not set, nothing should be traced...
      // but if it does, use stderr.
      out = stderr;
    }
  }
};

Init i;

const char* moduleName(Module mod) {
  return tokNames[mod];
}

void vtrace(const char *fmt, va_list ap) {
  static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
  if (moduleEnabledRelease(Trace::ringbuffer, 1)) {
    vtraceRingbuffer(fmt, ap);
  } else {
    vfprintf(out, fmt, ap);
    ONTRACE(1, pthread_mutex_lock(&mtx));
    ONTRACE(1, fprintf(out, "t%#08x: ", int(pthread_self())));
    fflush(out);
    ONTRACE(1, pthread_mutex_unlock(&mtx));
  }
}

void trace(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vtrace(fmt, ap);
  va_end(ap);
}

void traceRelease(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vtrace(fmt, ap);
  va_end(ap);
}

void trace(const std::string& s) {
  trace("%s", s.c_str());
}

template<>
std::string prettyNode(const char* name, const std::string& s) {
  using std::string;
  return string("(") + string(name) + string(" ") +
    s +
    string(")");
}

} } // HPHP::Trace

