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

#include <util/base.h>
#include <runtime/vm/translator/translator.h>
#include <runtime/vm/translator/translator-inline.h>
#include <runtime/vm/translator/annotation.h>

namespace HPHP {
namespace VM {
namespace Transl {

static const Trace::Module TRACEMOD = Trace::trans;

/*
 * A mapping from FCall instructions to the statically-known StringData*
 * that they're calling. Used to accelerate our FCall translations.
 */
enum CallRecordType {
  EncodedNameAndArgs,
  Function
};
struct CallRecord {
  CallRecordType m_type;
  union {
    const StringData* m_encodedName;
    const Func* m_func;
  };
};

typedef hphp_hash_map<SrcKey, CallRecord, SrcKey> CallDB;
static CallDB s_callDB;
/* record the max number of args to enable invalidation */
static int s_maxNumArgs;
int getMaxEncodedArgs() { return s_maxNumArgs; }

const StringData*
encodeCallAndArgs(const StringData* name, int numArgs) {
  char numArgsBuf[16];
  if (numArgs > s_maxNumArgs) s_maxNumArgs = numArgs;
  snprintf(numArgsBuf, 15, "@%d@", numArgs);
  String s = String(numArgsBuf) + String(name->data());
  return StringData::GetStaticString(s.get());
}

void recordNameAndArgs(const SrcKey& sk, const StringData* name, int numArgs) {
  CallRecord cr;
  cr.m_type = EncodedNameAndArgs;
  cr.m_encodedName = encodeCallAndArgs(name, numArgs);
  s_callDB.insert(std::make_pair(sk, cr));
}

void recordFunc(const SrcKey& sk, const Func* func) {
  CallRecord cr;
  cr.m_type = Function;
  cr.m_func = func;
  s_callDB.insert(std::make_pair(sk, cr));
}

static void recordActRecPush(const SrcKey& sk,
                             const Unit* unit,
                             const FPIEnt* fpi,
                             const StringData* name,
                             const StringData* clsName,
                             bool staticCall) {
  // sk is the address of a FPush* of the function whose static name
  // is name. The boundaries of FPI regions are such that we can't quite
  // find the FCall that matches this FuncD without decoding forward to
  // the end; this is not ideal, but is hopefully affordable at translation
  // time.
  ASSERT(name->isStatic());
  ASSERT(sk.offset() == fpi->m_fpushOff);
  SrcKey fcall;
  SrcKey next(sk);
  next.advance(unit);
  do {
    if (*unit->at(next.offset()) == OpFCall) {
      // Remember the last FCall in the region; the region might end
      // with UnboxR, e.g.
      fcall = next;
    }
    next.advance(unit);
  } while (next.offset() <= fpi->m_fcallOff);
  ASSERT(*unit->at(fcall.offset()) == OpFCall);
  if (clsName) {
    const Class* cls = Unit::lookupClass(clsName);
    bool magic = false;
    const Func* func = lookupImmutableMethod(cls, name, magic, staticCall);
    if (func) {
      recordFunc(fcall, func);
    }
    return;
  }
  const Func* func = Unit::lookupFunc(name);
  if (func && func->isNameBindingImmutable(unit)) {
    // this will never go into a call cache, so we dont need to
    // encode the args. it will be used in OpFCall below to
    // set the i->funcd.
    recordFunc(fcall, func);
  } else {
    // It's not enough to remember the function name; we also need to encode
    // the number of arguments and current flag disposition.
    int numArgs = getImm(unit->at(sk.offset()), 0).u_IVA;
    recordNameAndArgs(fcall, name, numArgs);
  }
}

void annotate(NormalizedInstruction* i) {
  switch(i->op()) {
    case OpFPushObjMethodD:
    case OpFPushClsMethodD:
    case OpFPushClsMethodF:
    case OpFPushFuncD: {
      // When we push predictable action records, we can use a simpler
      // translation for their corresponding FCall.
      SrcKey next(i->source);
      next.advance(curUnit());
      const StringData* className = NULL;
      const StringData* funcName = NULL;
      if (i->op() == OpFPushFuncD) {
      	funcName = curUnit()->lookupLitstrId(i->imm[1].u_SA);
      } else if (i->op() == OpFPushObjMethodD) {
        if (i->inputs[0]->valueType() != KindOfObject) break;
        const Class* cls = i->inputs[0]->rtt.valueClass();
        if (!cls) break;
        funcName = curUnit()->lookupLitstrId(i->imm[1].u_SA);
        className = cls->name();
      } else if (i->op() == OpFPushClsMethodF) {
        if (i->inputs[1]->rtt.valueString() == NULL ||
            i->inputs[0]->valueType() != KindOfClass) {
          break;
        }
        const Class* cls = i->inputs[0]->rtt.valueClass();
        if (!cls) break;
        funcName = i->inputs[1]->rtt.valueString();
        className = cls->name();
      } else {
        ASSERT(i->op() == OpFPushClsMethodD);
        funcName = curUnit()->lookupLitstrId(i->imm[1].u_SA);
        className = curUnit()->lookupLitstrId(i->imm[2].u_SA);
      }
      ASSERT(funcName->isStatic());
      const FPIEnt *fe = curFunc()->findFPI(next.m_offset);
      ASSERT(fe);
      recordActRecPush(i->source, curUnit(), fe, funcName, className,
                       i->op() == OpFPushClsMethodD ||
                       i->op() == OpFPushClsMethodF);
    } break;
    case OpFCall: {
      CallRecord callRec;
      if (mapGet(s_callDB, i->source, &callRec)) {
        if (callRec.m_type == Function) {
          i->funcd = callRec.m_func;
        } else {
          ASSERT(callRec.m_type == EncodedNameAndArgs);
          i->funcName = callRec.m_encodedName;
        }
      } else {
        i->funcName = NULL;
      }
    } break;
    default: break;
  }
}

} } }

