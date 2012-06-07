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

#ifndef incl_VM_BYTECODE_H_
#define incl_VM_BYTECODE_H_

#include "util/util.h"
#include "runtime/base/complex_types.h"
#include "runtime/base/class_info.h"
#include "runtime/base/array/array_iterator.h"

#include "runtime/ext/bcmath/bcmath.h" // for MAX(a,b)

// We want to use the TypedValue macros inside this header file, but we don't
// want to pollute the environment of files that include this header file.
// Thus, we record whether they were already defined here. That way, we know
// whether we need to undefine the macros at the end of this header file.
#ifndef __HPHP_TV_MACROS__
#include "runtime/base/tv_macros.h"
#define __VM_BYTECODE_H_SHOULD_UNDEF_TV__
#endif

#include "runtime/vm/core_types.h"
#include "runtime/vm/class.h"
#include "runtime/vm/instance.h"
#include "runtime/vm/unit.h"

namespace HPHP {

namespace VM {

#define LITSTR_DECREF(s) do {                                                 \
  if ((s)->decRefCount() == 0) {                                              \
    (s)->release();                                                           \
  }                                                                           \
} while (0)

// SETOP_BODY() would ideally be an inline function, but the header
// dependencies for concat_assign() make this unfeasible.
#define SETOP_BODY(lhs, op, rhs) do {                                         \
  switch (op) {                                                               \
  case SetOpPlusEqual: tvAsVariant(lhs) += tvCellAsCVarRef(rhs); break;       \
  case SetOpMinusEqual: tvAsVariant(lhs) -= tvCellAsCVarRef(rhs); break;      \
  case SetOpMulEqual: tvAsVariant(lhs) *= tvCellAsCVarRef(rhs); break;        \
  case SetOpDivEqual: tvAsVariant(lhs) /= tvCellAsCVarRef(rhs); break;        \
  case SetOpConcatEqual: {                                                    \
    concat_assign(tvAsVariant(lhs), tvCellAsCVarRef(rhs));                    \
    break;                                                                    \
  }                                                                           \
  case SetOpModEqual: tvAsVariant(lhs) %= tvCellAsCVarRef(rhs); break;        \
  case SetOpAndEqual: tvAsVariant(lhs) &= tvCellAsCVarRef(rhs); break;        \
  case SetOpOrEqual: tvAsVariant(lhs) |= tvCellAsCVarRef(rhs); break;         \
  case SetOpXorEqual: tvAsVariant(lhs) ^= tvCellAsCVarRef(rhs); break;        \
  case SetOpSlEqual: tvAsVariant(lhs) <<= tvCellAsCVarRef(rhs); break;        \
  case SetOpSrEqual: tvAsVariant(lhs) >>= tvCellAsCVarRef(rhs); break;        \
  default: ASSERT(false);                                                     \
  }                                                                           \
} while (0)

class Func;

// Variable environment.
//
// A variable environment consists of the locals for the current function
// (either pseudo-main or a normal function), plus any variables that are
// dynamically defined.  A normal (not pseudo-main) function starts off with
// a variable environment that contains only its locals, but a pseudo-main is
// handed its caller's existing variable environment.  We want local variable
// access to be fast for pseudo-mains, but we must maintain consistency with
// the associated variable environment.
//
// We achieve a consistent variable environment without sacrificing locals
// access performance by overlaying each pseudo-main's locals onto the
// current variable environment, so that at any given time, a variable in the
// variable environment has a canonical location (either a local variable on
// the stack, or a dynamically defined variable), which can only change at
// entry/exit of a pseudo-main.
class VarEnv {
 private:
  ActRec* m_cfp;
  HphpArray* m_name2info;
  std::vector<TypedValue**> m_restoreLocations;

  TypedValue* m_extraArgs;
  unsigned m_numExtraArgs;
  uint16_t m_depth;
  bool m_isGlobalScope;

 private:
  explicit VarEnv(); // create global fp
  explicit VarEnv(ActRec* fp); // attach to fp
  VarEnv(const VarEnv&);
  VarEnv& operator=(const VarEnv&);
  ~VarEnv();

 public:
  /*
   * Creates a VarEnv and attaches it to the existing frame fp.
   *
   * A lazy attach works by bringing all currently existing values for
   * the names in fp->m_func into the variable environment.  This is
   * used when we need a variable environment for some caller frame
   * (because we're about to attach a callee frame using attach()) but
   * don't actually have one.
   */
  static VarEnv* createLazyAttach(ActRec* fp, bool skipInsert = false);

  // Allocate a global VarEnv.  Initially not attached to any frame.
  static VarEnv* createGlobal();

  static void destroy(VarEnv*);

  void attach(ActRec* fp);
  void detach(ActRec* fp);

  void set(const StringData* name, TypedValue* tv);
  void bind(const StringData* name, TypedValue* tv);
  void setWithRef(const StringData* name, TypedValue* tv);
  TypedValue* lookup(const StringData* name);
  bool unset(const StringData* name);

  void setExtraArgs(TypedValue* args, unsigned nargs);
  void copyExtraArgs(TypedValue* args, unsigned nargs);
  unsigned numExtraArgs() const;
  TypedValue* getExtraArg(unsigned argInd) const;

  Array getDefinedVariables() const;

  // Used for save/store m_cfp for debugger
  void setCfp(ActRec* fp) { m_cfp = fp; }
  ActRec* getCfp() const { return m_cfp; }
  bool isGlobalScope() const { return m_isGlobalScope; }
};

/**
 * An "ActRec" is a call activation record. The order assumes that stacks
 * grow toward lower addresses.
 *
 * For most purposes, an ActRec can be considered to be in one of three
 * possible states:
 *   Pre-live:
 *     After the FPush* instruction which materialized the ActRec on the stack
 *     but before the corresponding FCall instruction
 *   Live:
 *     After the corresponding FCall instruction but before the ActRec fields
 *     and locals/iters have been decref'd (either by return or unwinding)
 *   Post-live:
 *     After the ActRec fields and locals/iters have been decref'd
 *
 * Note that when a function is invoked by the runtime via invokeFunc(), the
 * "pre-live" state is skipped and the ActRec is materialized in the "live"
 * state.
 */
struct ActRec {
  union {
    // This pair of uint64_t's must be the first two elements in the structure
    // so that the pointer to the ActRec can also be used for RBP chaining.
    // Note that ActRec's are also x64 frames, so this is an implicit machine
    // dependency.
    TypedValue _dummyA;
    struct {
      uint64_t m_savedRbp;     // Previous hardware frame pointer/ActRec.
      uint64_t m_savedRip;     // In-TC address to return to.
    };
  };
  union {
    TypedValue _dummyB;
    struct {
      const Func* m_func;      // Function.
      uint32_t m_soff;         // Saved offset of caller from beginning of
                               //   caller's Func's bytecode.

      // Bits 0-30 are the number of function args; the high bit is
      // whether this ActRec came from FPushCtor*.
      uint32_t m_numArgsAndCtorFlag;
    };
  };
  union {
    TypedValue m_r;          // Return value teleported here when the ActRec
                             //   is post-live.
    struct {
      union {
        ObjectData* m_this;    // This.
        Class* m_cls;          // Late bound class.
      };
      union {
        VarEnv* m_varEnv;      // Variable environment; only used when the
                               //   ActRec is live.
        StringData* m_invName; // Invoked function name (used for __call);
                               //   only used when ActRec is pre-live.
      };
    };
  };

  /**
   * Accessors for the packed m_numArgsAndCtorFlag field. We track
   * whether ActRecs came from FPushCtor* so that during unwinding we
   * can set the flag not to call destructors for objects whose
   * constructors exit via an exception.
   */

  int32_t numArgs() const {
    return m_numArgsAndCtorFlag & ~(1u << 31);
  }

  bool isFromFPushCtor() const {
    return m_numArgsAndCtorFlag & (1u << 31);
  }

  static inline uint32_t
  encodeNumArgs(uint32_t numArgs, bool isFPushCtor = false) {
    ASSERT((numArgs & (1u << 31)) == 0);
    return numArgs | (isFPushCtor << 31);
  }
  void initNumArgs(uint32_t numArgs, bool isFPushCtor = false) {
    m_numArgsAndCtorFlag = encodeNumArgs(numArgs, isFPushCtor);
  }

  void setNumArgs(uint32_t numArgs) {
    initNumArgs(numArgs, isFromFPushCtor());
  }

  /**
   * To conserve space, we use unions for pairs of mutually exclusive
   * fields (fields that are not used at the same time). We use unions
   * for m_this/m_cls and m_varEnv/m_invName.
   *
   * The least significant bit is used as a marker for each pair of fields
   * so that we can distinguish at runtime which field is valid. We define
   * accessors below to encapsulate this logic.
   *
   * Note that m_invName is only used when the ActRec is pre-live. Thus when
   * an ActRec is live it is safe to directly access m_varEnv without using
   * accessors.
   */

#define UNION_FIELD_ACCESSORS(name1, type1, field1, name2, type2, field2) \
  inline bool has##name1() const { \
    return field1 && !(intptr_t(field1) & 1LL); \
  } \
  inline bool has##name2() const { \
    return bool(intptr_t(field2) & 1LL); \
  } \
  inline type1 get##name1() const { \
    ASSERT(has##name1()); \
    return field1; \
  } \
  inline type2 get##name2() const { \
    ASSERT(has##name2()); \
    return (type2)(intptr_t(field2) & ~1LL); \
  } \
  inline void set##name1(type1 val) { \
    field1 = val; \
  } \
  inline void set##name2(type2 val) { \
    field2 = (type2)(intptr_t(val) | 1LL); \
  }

  UNION_FIELD_ACCESSORS(This, ObjectData*, m_this, Class, Class*, m_cls)
  UNION_FIELD_ACCESSORS(VarEnv, VarEnv*, m_varEnv, InvName, StringData*,
                        m_invName)

#undef UNION_FIELD_ACCESSORS
};

inline int32 arOffset(const ActRec* ar, const ActRec* other) {
  return (intptr_t(other) - intptr_t(ar)) / sizeof(TypedValue);
}

inline ActRec* arAtOffset(const ActRec* ar, int32 offset) {
  return (ActRec*)(intptr_t(ar) + intptr_t(offset * sizeof(TypedValue)));
}

inline ActRec* arFromSpOffset(const ActRec *sp, int32 offset) {
  return arAtOffset(sp, offset);
}

inline void arSetSfp(ActRec* ar, const ActRec* sfp) {
  CT_ASSERT(offsetof(ActRec, m_savedRbp) == 0);
  CT_ASSERT(sizeof(ActRec*) <= sizeof(uint64_t));
  ar->m_savedRbp = (uint64_t)sfp;
}

void IncDecBody(unsigned char op, TypedValue* fr, TypedValue* to);

template <bool crossBuiltin> Class* arGetContextClassImpl(const ActRec* ar);
#define arGetContextClass(ar)    HPHP::VM::arGetContextClassImpl<false>(ar)
#define arGetContextClassFromBuiltin(ar) \
  HPHP::VM::arGetContextClassImpl<true>(ar)

// Used by extension functions that take a PHP "callback", since they need to
// figure out the callback context once and call it multiple times. (e.g.
// array_map, array_filter, ...)
struct CallCtx {
  const Func* func;
  ObjectData* this_;
  Class* cls;
  StringData* invName;
};

struct Iter {
  enum Type {
    TypeUndefined,
    TypeArray,
    TypeMutableArray,
    TypeIterator  // for "implements Iterator" objects
  };
 private:
  // C++ won't let you have union members with constructors. So we get to
  // implement unions by hand.
  char m_u[MAX(sizeof(ArrayIter), sizeof(MIterCtx))];
 public:
  int32_t _pad;
  Type m_itype;
  MIterCtx& marr() {
    return *(MIterCtx*)m_u;
  }
  ArrayIter& arr() {
    return *(ArrayIter*)m_u;
  }
} __attribute__ ((aligned(16)));

static const size_t kNumIterCells = sizeof(Iter) / sizeof(Cell);
static const size_t kNumActRecCells = sizeof(ActRec) / sizeof(Cell);

struct Fault {
  enum FaultType {
    KindOfUserException,
    KindOfCPPException
  };
  FaultType m_faultType;
  union {
    ObjectData* m_userException;
    Exception* m_cppException;
  };
};

enum UnwindStatus {
  UnwindResumeVM,
  UnwindPropagate,
};

// Interpreter evaluation stack.
class Stack {
private:
  TypedValue* m_elms;
  TypedValue* m_top;
  TypedValue* m_base; // Stack grows down, so m_base is beyond the end of
                      // m_elms.

public:
  void* getStackLowAddress() const { return m_elms; }
  void* getStackHighAddress() const { return m_base; }
  bool isValidAddress(uintptr_t v) {
    return v >= uintptr_t(m_elms) && v < uintptr_t(m_base);
  }
  void toStringElm(std::ostream& os, TypedValue* vv, const ActRec* fp)
    const;
  void toStringIter(std::ostream& os, Iter* it) const;
  void clearEvalStack(ActRec* fp, int32 numLocals);
  void protect();
  void unprotect();
  void requestInit();
  void requestExit();
  static void flush();
private:
  void toStringFrag(std::ostream& os, const ActRec* fp,
                    const TypedValue* top) const;
  void toStringAR(std::ostream& os, const ActRec* fp,
                  const FPIEnt *fe, const TypedValue* top) const;
  void toStringFragAR(std::ostream& os, const ActRec* fp,
                    int offset, const TypedValue* top) const;
  void toStringFrame(std::ostream& os, const ActRec* fp,
                     int offset, const TypedValue* ftop,
                     const std::string& prefix) const;

  UnwindStatus unwindFrag(ActRec* fp, int offset, PC& pc, Fault& f);
  void unwindARFrag(ActRec* ar);
  void unwindAR(ActRec* fp, int offset, const FPIEnt* fe);
public:
  static const int sSurprisePageSize;
  static const uint sMinStackElms;
  static void ValidateStackSize();
  Stack();
  ~Stack();

  std::string toString(const ActRec* fp, int offset,
                       std::string prefix="") const;

  UnwindStatus unwindFrame(ActRec*& fp, int offset, PC& pc, Fault& f);

  bool wouldOverflow(int numCells) const;

  /*
   * top --
   * topOfStackOffset --
   *
   *   Accessors for the x64 translator. Do not play on or around.
   */
  TypedValue*& top() {
    return m_top;
  }

  static inline size_t topOfStackOffset() {
    Stack *that = 0;
    return (size_t)&that->m_top;
  }

  inline size_t ALWAYS_INLINE count() const {
    return ((uintptr_t)m_base - (uintptr_t)m_top) / sizeof(TypedValue);
  }

  // Same as discard(), but meant to replace popC() iff the interpreter knows
  // for certain that decrementing a refcount is unnecessary.
  inline void ALWAYS_INLINE popX() {
    ASSERT(m_top != m_base);
    ASSERT(!IS_REFCOUNTED_TYPE(m_top->m_type));
    m_top++;
  }

  inline void ALWAYS_INLINE popC() {
    ASSERT(m_top != m_base);
    ASSERT(m_top->m_type != KindOfVariant);
    tvRefcountedDecRefCell(m_top);
    m_top++;
  }

  inline void ALWAYS_INLINE popV() {
    ASSERT(m_top != m_base);
    ASSERT(m_top->m_type == KindOfVariant);
    ASSERT(m_top->m_data.ptv != NULL);
    tvDecRefVar(m_top);
    m_top++;
  }

  inline void ALWAYS_INLINE popTV() {
    ASSERT(m_top != m_base);
    tvRefcountedDecRef(m_top);
    m_top++;
  }

  // popAR() should only be used to tear down a pre-live ActRec. Once
  // an ActRec is live, it should be torn down using frame_free_locals()
  // followed by discardAR() or ret().
  inline void ALWAYS_INLINE popAR() {
    ASSERT(m_top != m_base);
    ActRec* ar = (ActRec*)m_top;
    if (ar->hasThis()) {
      ObjectData* this_ = ar->getThis();
      if (this_->decRefCount() == 0) {
        this_->release();
      }
    }
    if (ar->hasInvName()) {
      StringData* invName = ar->getInvName();
      if (invName->decRefCount() == 0) {
      }
    }
    m_top += kNumActRecCells;
    ASSERT((uintptr_t)m_top <= (uintptr_t)m_base);
  }

  inline void ALWAYS_INLINE discardAR() {
    ASSERT(m_top != m_base);
    m_top += kNumActRecCells;
    ASSERT((uintptr_t)m_top <= (uintptr_t)m_base);
  }

  inline void ALWAYS_INLINE ret() {
    // Leave part of the activation on the stack, since the return value now
    // resides there.
    m_top += kNumActRecCells - 1;
    ASSERT((uintptr_t)m_top <= (uintptr_t)m_base);
  }

  inline void ALWAYS_INLINE discard() {
    ASSERT(m_top != m_base);
    m_top++;
  }

  inline void ALWAYS_INLINE ndiscard(size_t n) {
    ASSERT((uintptr_t)&m_top[n] <= (uintptr_t)m_base);
    m_top += n;
  }

  inline void ALWAYS_INLINE dup() {
    ASSERT(m_top != m_base);
    ASSERT(m_top != m_elms);
    ASSERT(m_top->m_type != KindOfVariant);
    Cell* fr = (Cell*)m_top;
    m_top--;
    Cell* to = (Cell*)m_top;
    TV_DUP_CELL(fr, to);
  }

  inline void ALWAYS_INLINE box() {
    ASSERT(m_top != m_base);
    ASSERT(m_top->m_type != KindOfVariant);
    tvBox(m_top);
  }

  inline void ALWAYS_INLINE unbox() {
    ASSERT(m_top != m_base);
    ASSERT(m_top->m_type == KindOfVariant);
    TV_UNBOX(m_top);
  }

  inline void ALWAYS_INLINE pushUninit() {
    ASSERT(m_top != m_elms);
    m_top--;
    TV_WRITE_UNINIT(m_top);
  }

  inline void ALWAYS_INLINE pushNull() {
    ASSERT(m_top != m_elms);
    m_top--;
    TV_WRITE_NULL(m_top);
  }

  #define PUSH_METHOD(name, type, field, value)                               \
  inline void ALWAYS_INLINE push##name() {                                    \
    ASSERT(m_top != m_elms);                                                  \
    m_top--;                                                                  \
    m_top->m_data.field = value;                                              \
    m_top->_count = 0;                                                        \
    m_top->m_type = type;                                                     \
  }
  PUSH_METHOD(True, KindOfBoolean, num, 1)
  PUSH_METHOD(False, KindOfBoolean, num, 0)

  #define PUSH_METHOD_ARG(name, type, field, argtype, arg)                    \
  inline void ALWAYS_INLINE push##name(argtype arg) {                         \
    ASSERT(m_top != m_elms);                                                  \
    m_top--;                                                                  \
    m_top->m_data.field = arg;                                                \
    m_top->_count = 0;                                                        \
    m_top->m_type = type;                                                     \
  }
  PUSH_METHOD_ARG(Int, KindOfInt64, num, int64, i)
  PUSH_METHOD_ARG(Double, KindOfDouble, dbl, double, d)

  // This should only be called directly when the caller has
  // already adjusted the refcount appropriately
  inline void ALWAYS_INLINE pushStringNoRc(StringData* s) {
    ASSERT(m_top != m_elms);
    m_top--;
    m_top->m_data.pstr = s;
    m_top->_count = 0;
    m_top->m_type = KindOfString;
  }

  inline void ALWAYS_INLINE pushStaticString(StringData* s) {
    ASSERT(s->isStatic()); // No need to call s->incRefCount().
    pushStringNoRc(s);
  }

  // This should only be called directly when the caller has
  // already adjusted the refcount appropriately
  inline void ALWAYS_INLINE pushArrayNoRc(ArrayData* a) {
    ASSERT(m_top != m_elms);
    m_top--;
    m_top->m_data.parr = a;
    m_top->_count = 0;
    m_top->m_type = KindOfArray;
  }

  inline void ALWAYS_INLINE pushArray(ArrayData* a) {
    pushArrayNoRc(a);
    a->incRefCount();
  }

  inline void ALWAYS_INLINE pushStaticArray(ArrayData* a) {
    ASSERT(a->isStatic()); // No need to call a->incRefCount().
    pushArrayNoRc(a);
  }

  // This should only be called directly when the caller has
  // already adjusted the refcount appropriately
  inline void ALWAYS_INLINE pushObjectNoRc(ObjectData* o) {
    ASSERT(m_top != m_elms);
    m_top--;
    m_top->m_data.pobj = o;
    m_top->_count = 0;
    m_top->m_type = KindOfObject;
  }

  inline void ALWAYS_INLINE pushObject(ObjectData* o) {
    pushObjectNoRc(o);
    o->incRefCount();
  }

  inline Cell* ALWAYS_INLINE allocC() {
    ASSERT(m_top != m_elms);
    m_top--;
    return (Cell*)m_top;
  }

  inline Var* ALWAYS_INLINE allocV() {
    ASSERT(m_top != m_elms);
    m_top--;
    return (Var*)m_top;
  }

  inline TypedValue* ALWAYS_INLINE allocTV() {
    ASSERT(m_top != m_elms);
    m_top--;
    return m_top;
  }

  inline ActRec* ALWAYS_INLINE allocA() {
    ASSERT((uintptr_t)&m_top[-kNumActRecCells] >= (uintptr_t)m_elms);
    ASSERT(kNumActRecCells * sizeof(Cell) == sizeof(ActRec));
    m_top -= kNumActRecCells;
    return (ActRec*)m_top;
  }

  inline void ALWAYS_INLINE allocI() {
    ASSERT(kNumIterCells * sizeof(Cell) == sizeof(Iter));
    ASSERT((uintptr_t)&m_top[-kNumIterCells] >= (uintptr_t)m_elms);
    m_top -= kNumIterCells;
  }

  inline Cell* ALWAYS_INLINE topC() {
    ASSERT(m_top != m_base);
    ASSERT(m_top->m_type != KindOfVariant);
    return (Cell*)m_top;
  }

  inline Var* ALWAYS_INLINE topV() {
    ASSERT(m_top != m_base);
    ASSERT(m_top->m_type == KindOfVariant);
    return (Var*)m_top;
  }

  inline TypedValue* ALWAYS_INLINE topTV() {
    ASSERT(m_top != m_base);
    return m_top;
  }

  inline Cell* ALWAYS_INLINE indC(size_t ind) {
    ASSERT(m_top != m_base);
    ASSERT(m_top[ind].m_type != KindOfVariant);
    return (Cell*)(&m_top[ind]);
  }

  inline TypedValue* ALWAYS_INLINE indTV(size_t ind) {
    ASSERT(m_top != m_base);
    return &m_top[ind];
  }
  inline void ALWAYS_INLINE pushClass(Class* clss) {
    ASSERT(m_top != m_elms);
    m_top--;
    m_top->m_data.pcls = clss;
    m_top->_count = 0;
    m_top->m_type = KindOfClass;
  }
};

///////////////////////////////////////////////////////////////////////////////
}
}

// Undefine the TypedValue macros if appropriate
#ifdef __VM_BYTECODE_H_SHOULD_UNDEF_TV__
#undef __VM_BYTECODE_H_SHOULD_UNDEF_TV__
#include <runtime/base/undef_tv_macros.h>
#endif

#endif // __VM_BYTECODE_H__
