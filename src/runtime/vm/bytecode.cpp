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

#include <iostream>
#include <iomanip>
#include <boost/format.hpp>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include <libgen.h>
#include <sys/mman.h>

#include <compiler/builtin_symbols.h>
#include <runtime/vm/bytecode.h>
#include <runtime/vm/event_hook.h>
#include <runtime/vm/translator/translator-deps.h>
#include <runtime/vm/translator/translator-x64.h>
#include <runtime/base/code_coverage.h>
#include <runtime/eval/runtime/file_repository.h>
#include <runtime/base/base_includes.h>
#include <runtime/base/execution_context.h>
#include <runtime/base/runtime_option.h>
#include <runtime/base/array/hphp_array.h>
#include <runtime/base/strings.h>
#include <util/util.h>
#include <util/trace.h>
#include <util/debug.h>
#include <util/stat_cache.h>

#include <runtime/base/tv_macros.h>
#include <runtime/vm/instrumentation_hook.h>
#include <runtime/vm/php_debug.h>
#include <runtime/vm/debugger_hook.h>
#include <runtime/vm/runtime.h>
#include <runtime/vm/translator/targetcache.h>
#include <runtime/vm/type_constraint.h>
#include <runtime/vm/translator/translator-inline.h>
#include <runtime/ext/profile/extprofile_string.h>
#include <runtime/ext/ext_continuation.h>
#include <runtime/ext/ext_function.h>
#include <runtime/ext/ext_variable.h>
#include <runtime/vm/exception_gate.h>
#include <runtime/vm/stats.h>
#include <runtime/base/server/source_root_info.h>
#include <runtime/base/util/extended_logger.h>

#include <system/lib/systemlib.h>

using std::string;

namespace HPHP {

// RepoAuthoritative has been raptured out of runtime_option.cpp. It needs
// to be closer to other bytecode.cpp data.
bool RuntimeOption::RepoAuthoritative = false;

namespace VM {

#if DEBUG
#define OPTBLD_INLINE
#else
#define OPTBLD_INLINE ALWAYS_INLINE
#endif
static const Trace::Module TRACEMOD = Trace::bcinterp;

template <>
Class* arGetContextClassImpl<false>(const ActRec* ar) {
  if (ar == NULL) {
    return NULL;
  }
  if (ar->m_func->isPseudoMain()) {
    // Pseudomains inherit the context of their caller
    VMExecutionContext* context = g_vmContext;
    ar = context->getPrevVMState(ar);
    while (ar != NULL && ar->m_func->isPseudoMain()) {
      ar = context->getPrevVMState(ar);
    }
    if (ar == NULL) {
      return NULL;
    }
  }
  return ar->m_func->cls();
}

template <>
Class* arGetContextClassImpl<true>(const ActRec* ar) {
  if (ar == NULL) {
    return NULL;
  }
  if (ar->m_func->isPseudoMain() || ar->m_func->isBuiltin()) {
    // Pseudomains inherit the context of their caller
    VMExecutionContext* context = g_vmContext;
    ar = context->getPrevVMState(ar);
    while (ar != NULL &&
             (ar->m_func->isPseudoMain() || ar->m_func->isBuiltin())) {
      ar = context->getPrevVMState(ar);
    }
    if (ar == NULL) {
      return NULL;
    }
  }
  return ar->m_func->cls();
}

// When MoreWarnings is set to true, the VM will match Zend on
// warnings for SetOpM and CGetG
static const bool MoreWarnings = false;

static StaticString s_call_user_func(LITSTR_INIT("call_user_func"));
static StaticString s_call_user_func_array(LITSTR_INIT("call_user_func_array"));
static StaticString s_hphpd_break(LITSTR_INIT("hphpd_break"));
static StaticString s_fb_enable_code_coverage(
  LITSTR_INIT("fb_enable_code_coverage"));
static StaticString s_file(LITSTR_INIT("file"));
static StaticString s_line(LITSTR_INIT("line"));
static StaticString s_stdclass(LITSTR_INIT("stdclass"));
static StaticString s___call(LITSTR_INIT("__call"));
static StaticString s___callStatic(LITSTR_INIT("__callStatic"));

///////////////////////////////////////////////////////////////////////////////

//=============================================================================
// Miscellaneous macros.

#define NEXT() pc++
#define DECODE_JMP(type, var)                                                 \
  type var __attribute__((unused)) = *(type*)pc;                              \
  ONTRACE(1,                                                                  \
          Trace::trace("decode:     Immediate %s %"PRIi64"\n", #type,         \
                       (int64_t)var));
#define ITER_SKIP(offset)  pc = origPc + (offset);

#define DECODE(type, var)                                                     \
  DECODE_JMP(type, var);                                                      \
  pc += sizeof(type)
#define DECODE_IVA(var)                                                       \
  int32 var UNUSED = decodeVariableSizeImm(&pc);                              \
  ONTRACE(1,                                                                  \
          Trace::trace("decode:     Immediate int32 %"PRIi64"\n",             \
                       (int64_t)var));
#define DECODE_LITSTR(var)                                \
  StringData* var;                                        \
  do {                                                    \
    DECODE(Id, id);                                       \
    var = m_fp->m_func->unit()->lookupLitstrId(id);       \
  } while (false)

#define DECODE_HA(var) DECODE_IVA(var)
#define DECODE_IA(var) DECODE_IVA(var)

#define SYNC() m_pc = pc

//=============================================================================
// Miscellaneous helpers.

static void
objArrayAccess(TypedValue* base) {
  ASSERT(base->m_type == KindOfObject);
  if (!instanceOf(tvAsCVarRef(base), "ArrayAccess")) {
    raise_error("Object does not implement ArrayAccess");
  }
}

static TypedValue*
objOffsetGet(TypedValue& tvRef, TypedValue* base,
             CVarRef offset, bool validate=true) {
  if (validate) {
    objArrayAccess(base);
  }
  TypedValue* result;
  ObjectData* obj = base->m_data.pobj;
  if (LIKELY(obj->isInstance())) {
    Instance* instance = static_cast<Instance*>(obj);
    static StringData* sd__offsetGet = StringData::GetStaticString("offsetGet");
    const Func* method = instance->methodNamed(sd__offsetGet);
    ASSERT(method != NULL);
    instance->invokeUserMethod(&tvRef, method, CREATE_VECTOR1(offset));
    result = &tvRef;
  } else {
    tvAsVariant(&tvRef)
      = tvAsVariant(base).getArrayAccess()->___offsetget_lval(offset);
    result = &tvRef;
  }
  return result;
}

static bool
objOffsetExists(TypedValue* base, CVarRef offset) {
  objArrayAccess(base);
  TypedValue tvResult;
  tvWriteUninit(&tvResult);
  static StringData* sd__offsetExists
    = StringData::GetStaticString("offsetExists");
  ObjectData* obj = base->m_data.pobj;
  if (LIKELY(obj->isInstance())) {
    Instance* instance = static_cast<Instance*>(obj);
    const Func* method = instance->methodNamed(sd__offsetExists);
    ASSERT(method != NULL);
    instance->invokeUserMethod(&tvResult, method, CREATE_VECTOR1(offset));
  } else {
    tvAsVariant(&tvResult) = tvAsVariant(base).getArrayAccess()
      ->o_invoke(sd__offsetExists, CREATE_VECTOR1(offset));
  }
  tvCastToBooleanInPlace(&tvResult);
  return bool(tvResult.m_data.num);
}

static void
objOffsetSet(TypedValue* base, CVarRef offset, TypedValue* val,
             bool validate=true) {
  if (validate) {
    objArrayAccess(base);
  }
  static StringData* sd__offsetSet = StringData::GetStaticString("offsetSet");
  ObjectData* obj = base->m_data.pobj;
  if (LIKELY(obj->isInstance())) {
    Instance* instance = static_cast<Instance*>(obj);
    const Func* method = instance->methodNamed(sd__offsetSet);
    ASSERT(method != NULL);
    TypedValue tvResult;
    tvWriteUninit(&tvResult);
    instance->invokeUserMethod(&tvResult, method,
                               CREATE_VECTOR2(offset, tvAsCVarRef(val)));
    tvRefcountedDecRef(&tvResult);
  } else {
    tvAsVariant(base).getArrayAccess()
      ->o_invoke(sd__offsetSet, CREATE_VECTOR2(offset, tvAsCVarRef(val)));
  }
}

static void
objOffsetUnset(TypedValue* base, CVarRef offset) {
  objArrayAccess(base);
  static StringData* sd__offsetUnset
    = StringData::GetStaticString("offsetUnset");
  ObjectData* obj = base->m_data.pobj;
  if (LIKELY(obj->isInstance())) {
    Instance* instance = static_cast<Instance*>(obj);
    const Func* method = instance->methodNamed(sd__offsetUnset);
    ASSERT(method != NULL);
    TypedValue tv;
    tvWriteUninit(&tv);
    instance->invokeUserMethod(&tv, method, CREATE_VECTOR1(offset));
    tvRefcountedDecRef(&tv);
  } else {
    tvAsVariant(base).getArrayAccess()
      ->o_invoke(sd__offsetUnset, CREATE_VECTOR1(offset));
  }
}

static inline String prepareKey(TypedValue* tv) {
  String ret;
  if (IS_STRING_TYPE(tv->m_type)) {
    ret = tv->m_data.pstr;
  } else {
    ret = tvAsCVarRef(tv).toString();
  }
  return ret;
}

static inline void opPre(TypedValue*& base, DataType& type) {
  // Get inner variant if necessary.
  type = base->m_type;
  if (type == KindOfVariant) {
    base = base->m_data.ptv;
    type = base->m_type;
  }
}

template <bool warn>
static inline TypedValue* ElemArray(TypedValue* base,
                                    TypedValue* key) {
  TypedValue* result;
  bool isHphpArray = IsHphpArray(base->m_data.parr);
  if (key->m_type == KindOfInt64) {
    if (LIKELY(isHphpArray)) {
      result = (static_cast<HphpArray*>(base->m_data.parr))
               ->nvGet(key->m_data.num);
      if (result == NULL) {
        result = (TypedValue*)&null_variant;
      }
    } else {
      result = (TypedValue*)&tvCellAsVariant(base).asArrRef()
        .rvalAtRef(key->m_data.num);
    }
  } else if (IS_STRING_TYPE(key->m_type)) {
    if (LIKELY(isHphpArray)) {
      int64 n;
      if (!key->m_data.pstr->isStrictlyInteger(n)) {
        result = (static_cast<HphpArray*>(base->m_data.parr))
                 ->nvGet(key->m_data.pstr);
      } else {
        result = (static_cast<HphpArray*>(base->m_data.parr))->nvGet(n);
      }

      if (result == NULL) {
        result = (TypedValue*)&null_variant;
      }
    } else {
      result = (TypedValue*)&tvCellAsVariant(base).asArrRef()
        .rvalAtRef(tvCellAsVariant(key).asCStrRef());
    }
  } else {
    result = (TypedValue*)&tvCellAsVariant(base).asArrRef()
      .rvalAtRef(tvCellAsCVarRef(key));
  }

  if (UNLIKELY(result->m_type == KindOfUninit)) {
    result = (TypedValue*)&init_null_variant;
    if (warn) {
      raise_notice(Strings::UNDEFINED_INDEX,
                   tvAsCVarRef(key).toString().data());
    }
  }
  return result;
}

// $result = $base[$key];
template <bool warn>
static inline TypedValue* Elem(TypedValue& tvScratch, TypedValue& tvRef,
                               TypedValue* base, bool& baseStrOff,
                               TypedValue* key) {
  TypedValue* result;
  DataType type;
  opPre(base, type);
  switch (type) {
  case KindOfUninit:
  case KindOfNull: {
    result = (TypedValue*)&init_null_variant;
    break;
  }
  case KindOfBoolean: {
    result = (TypedValue*)&init_null_variant;
    break;
  }
  case KindOfInt32:
  case KindOfInt64: {
    result = (TypedValue*)&init_null_variant;
    break;
  }
  case KindOfDouble: {
    result = (TypedValue*)&init_null_variant;
    break;
  }
  case KindOfStaticString:
  case KindOfString: {
    if (baseStrOff) {
      raise_error("Cannot use string offset as an array");
    }
    int64 x = IS_INT_TYPE(key->m_type)
              ? key->m_data.num
              : int64(tvCellAsCVarRef(key));
    if (x < 0 || x >= base->m_data.pstr->size()) {
      if (warn) {
        raise_warning("Out of bounds");
      }
      static StringData* sd = StringData::GetStaticString("");
      tvScratch.m_data.pstr = sd;
      tvScratch._count = 0;
      tvScratch.m_type = KindOfString;
    } else {
      tvAsVariant(&tvScratch) = base->m_data.pstr->getChar(x);
    }
    result = &tvScratch;
    baseStrOff = true;
    break;
  }
  case KindOfArray: {
    result = ElemArray<warn>(base, key);
    break;
  }
  case KindOfObject: {
    result = objOffsetGet(tvRef, base, tvCellAsCVarRef(key));
    break;
  }
  default: {
    ASSERT(false);
    result = NULL;
  }
  }
  return result;
}

template <bool warn>
static inline TypedValue* ElemDArray(TypedValue* base, TypedValue* key) {
  TypedValue* result;
  bool defined = !warn || tvAsVariant(base).asArrRef()
                          .exists(tvAsCVarRef(key));
  if (key->m_type == KindOfInt64) {
    result = (TypedValue*)&tvAsVariant(base).asArrRef()
                                            .lvalAt(key->m_data.num);
  } else {
    result = (TypedValue*)&tvAsVariant(base).asArrRef()
                                            .lvalAt(tvCellAsCVarRef(key));
  }

  if (warn) {
    if (!defined) {
      raise_notice(Strings::UNDEFINED_INDEX,
                   tvAsCVarRef(key).toString().data());
    }
  }

  return result;
}

// $base[$key] = ...
// \____ ____/
//      v
//   $result
template <bool warn>
static inline TypedValue* ElemD(TypedValue& tvScratch, TypedValue& tvRef,
                                TypedValue* base, TypedValue* key) {
  TypedValue* result;
  DataType type;
  opPre(base, type);
  switch (type) {
  case KindOfUninit:
  case KindOfNull: {
    Array a = Array::Create();
    result = (TypedValue*)&a.lvalAt(tvCellAsCVarRef(key));
    if (warn) {
      raise_notice(Strings::UNDEFINED_INDEX,
                   tvAsCVarRef(key).toString().data());
    }
    tvAsVariant(base) = a;
    break;
  }
  case KindOfBoolean: {
    if (base->m_data.num) {
      raise_warning(Strings::CANNOT_USE_SCALAR_AS_ARRAY);
      tvWriteUninit(&tvScratch);
      result = &tvScratch;
    } else {
      Array a = Array::Create();
      result = (TypedValue*)&a.lvalAt(tvCellAsCVarRef(key));
      if (warn) {
        raise_notice(Strings::UNDEFINED_INDEX,
                     tvAsCVarRef(key).toString().data());
      }
      tvAsVariant(base) = a;
    }
    break;
  }
  case KindOfInt32:
  case KindOfInt64:
  case KindOfDouble: {
    raise_warning(Strings::CANNOT_USE_SCALAR_AS_ARRAY);
    tvWriteUninit(&tvScratch);
    result = &tvScratch;
    break;
  }
  case KindOfStaticString:
  case KindOfString: {
    if (base->m_data.pstr->size() == 0) {
      Array a = Array::Create();
      result = (TypedValue*)&a.lvalAt(tvCellAsCVarRef(key));
      if (warn) {
        raise_notice(Strings::UNDEFINED_INDEX,
                     tvAsCVarRef(key).toString().data());
      }
      tvAsVariant(base) = a;
    } else {
      raise_error("Operator not supported for strings");
      result = NULL; // Silence compiler warning.
    }
    break;
  }
  case KindOfArray: {
    result = ElemDArray<warn>(base, key);
    break;
  }
  case KindOfObject: {
    result = objOffsetGet(tvRef, base, tvCellAsCVarRef(key));
    break;
  }
  default: {
    ASSERT(false);
    result = NULL; // Silence compiler warning.
  }
  }
  return result;
}

// $base[$key] = ...
// \____ ____/
//      v
//   $result
static inline TypedValue* ElemU(TypedValue& tvScratch, TypedValue& tvRef,
                                TypedValue* base, TypedValue* key) {
  TypedValue* result = NULL;
  DataType type;
  opPre(base, type);
  switch (type) {
  case KindOfUninit:
  case KindOfNull:
  case KindOfBoolean:
  case KindOfInt32:
  case KindOfInt64:
  case KindOfDouble: {
    tvWriteUninit(&tvScratch);
    result = &tvScratch;
    break;
  }
  case KindOfStaticString:
  case KindOfString: {
    raise_error("Operator not supported for strings");
    break;
  }
  case KindOfArray: {
    bool defined = tvAsVariant(base).asArrRef()
                      .exists(tvAsCVarRef(key));
    if (defined) {
      if (key->m_type == KindOfInt64) {
        result = (TypedValue*)&tvAsVariant(base).asArrRef()
                                                .lvalAt(key->m_data.num);
      } else {
        result = (TypedValue*)&tvAsVariant(base).asArrRef()
                                                .lvalAt(tvCellAsCVarRef(key));
      }
    } else {
      tvWriteUninit(&tvScratch);
      result = &tvScratch;
    }
    break;
  }
  case KindOfObject: {
    result = objOffsetGet(tvRef, base, tvCellAsCVarRef(key));
    break;
  }
  default: {
    ASSERT(false);
    result = NULL;
  }
  }
  return result;
}

// $result = ($base[] = ...);
static TypedValue* NewElem(TypedValue& tvScratch, TypedValue& tvRef,
                           TypedValue* base) {
  TypedValue* result;
  DataType type;
  opPre(base, type);
  switch (type) {
  case KindOfUninit:
  case KindOfNull: {
    Array a = Array::Create();
    result = (TypedValue*)&a.lvalAt();
    tvAsVariant(base) = a;
    break;
  }
  case KindOfBoolean: {
    if (base->m_data.num) {
      raise_warning("Invalid NewElem operand");
      tvWriteUninit(&tvScratch);
      result = &tvScratch;
    } else {
      Array a = Array::Create();
      result = (TypedValue*)&a.lvalAt();
      tvAsVariant(base) = a;
    }
    break;
  }
  case KindOfStaticString:
  case KindOfString: {
    if (base->m_data.pstr->size() == 0) {
      Array a = Array::Create();
      result = (TypedValue*)&a.lvalAt();
      tvAsVariant(base) = a;
    } else {
      raise_warning("Invalid NewElem operand");
      tvWriteUninit(&tvScratch);
      result = &tvScratch;
    }
    break;
  }
  case KindOfArray: {
    result = (TypedValue*)&tvAsVariant(base).asArrRef().lvalAt();
    break;
  }
  case KindOfObject: {
    result = objOffsetGet(tvRef, base, null_variant);
    break;
  }
  default: {
    raise_warning("Invalid NewElem operand");
    tvWriteUninit(&tvScratch);
    result = &tvScratch;
    break;
  }
  }
  return result;
}

static inline void SetElemEmptyish(TypedValue* base, TypedValue* key,
                                   Cell* value) {
  Array a = Array::Create();
  TypedValue* result = (TypedValue*)&a.lvalAt(tvAsCVarRef(key));
  tvAsVariant(base) = a;
  tvDupCell((TypedValue*)value, result);
}
static inline void SetElemNumberish(Cell* value) {
  raise_warning(Strings::CANNOT_USE_SCALAR_AS_ARRAY);
  tvRefcountedDecRefCell((TypedValue*)value);
  tvWriteNull((TypedValue*)value);
}
// SetElem() leaves the result in 'value', rather than returning it as in
// SetOpElem(), because doing so avoids a dup operation that SetOpElem() can't
// get around.
static inline void SetElem(TypedValue* base, TypedValue* key, Cell* value) {
  DataType type;
  opPre(base, type);
  switch (type) {
  case KindOfUninit:
  case KindOfNull: {
    SetElemEmptyish(base, key, value);
    break;
  }
  case KindOfBoolean: {
    if (base->m_data.num) {
      SetElemNumberish(value);
    } else {
      SetElemEmptyish(base, key, value);
    }
    break;
  }
  case KindOfInt32:
  case KindOfInt64:
  case KindOfDouble: {
    SetElemNumberish(value);
    break;
  }
  case KindOfStaticString:
  case KindOfString: {
    int baseLen = base->m_data.pstr->size();
    if (baseLen == 0) {
      SetElemEmptyish(base, key, value);
    } else {
      // Convert key to string offset.
      int64 x;
      {
        TypedValue tv;
        tvDup(key, &tv);
        tvCastToInt64InPlace(&tv);
        x = tv.m_data.num;
      }
      if (x < 0) {
        raise_warning("Illegal string offset: %lld", x);
        break;
      }
      // Compute how long the resulting string will be.
      int slen;
      if (x >= baseLen) {
        slen = x + 1;
      } else {
        slen = baseLen;
      }
      // Extract the first character of (string)value.
      char y[2];
      {
        TypedValue tv;
        tvDup(value, &tv);
        tvCastToStringInPlace(&tv);
        if (tv.m_data.pstr->size() > 0) {
          y[0] = tv.m_data.pstr->data()[0];
          y[1] = '\0';
        } else {
          y[0] = '\0';
        }
        tvRefcountedDecRef(&tv);
      }
      // Create and save the result.
      if (x >= 0 && x < baseLen && base->m_data.pstr->getCount() <= 1) {
        // Modify base in place.  This is safe because the LHS owns the only
        // reference.
        base->m_data.pstr->setChar(x, y[0]);
      } else {
        char* s = (char*)malloc(slen + 1);
        if (s == NULL) {
          raise_error("Out of memory");
        }
        memcpy(s, base->m_data.pstr->data(), baseLen);
        if (x > baseLen) {
          memset(&s[baseLen], ' ', slen - baseLen - 1);
        }
        s[x] = y[0];
        s[slen] = '\0';
        StringData* sd = NEW(StringData)(s, slen, AttachString);
        sd->incRefCount();
        base->m_data.pstr = sd;
        base->m_type = KindOfString;
      }
      // Push y onto the stack.
      tvRefcountedDecRef(value);
      StringData* sd = NEW(StringData)(y, strlen(y), CopyString);
      sd->incRefCount();
      value->m_data.pstr = sd;
      value->_count = 0;
      value->m_type = KindOfString;
    }
    break;
  }
  case KindOfArray: {
    ArrayData* a = base->m_data.parr;
    ArrayData* newData = NULL;
    bool copy = (a->getCount() > 1)
                || (value->m_type == KindOfArray && value->m_data.parr == a);
    ASSERT(key->m_type != KindOfVariant);
    if (key->m_type <= KindOfNull) {
      newData = a->set(empty_string, tvCellAsCVarRef(value), copy);
    } else if (IS_STRING_TYPE(key->m_type)) {
      int64 n;
      if (key->m_data.pstr->isStrictlyInteger(n)) {
        newData = a->set(n, tvCellAsCVarRef(value), copy);
      } else {
        newData = a->set(tvAsCVarRef(key), tvCellAsCVarRef(value), copy);
      }
    } else if (key->m_type != KindOfArray && key->m_type != KindOfObject) {
      newData = a->set(tvAsCVarRef(key).toInt64(), tvCellAsCVarRef(value),
                       copy);
    } else {
      raise_warning("Illegal offset type");
      // Assignment failed, so the result is null rather than the RHS.
      // XXX This does not match bytecode.specification, but it does roughly
      // match Zend and hphpi behavior.
      if (IS_REFCOUNTED_TYPE(value->m_type)) {
        tvDecRef(value);
      }
      tvWriteNull(value);
    }

    if (newData != NULL && newData != a) {
      newData->incRefCount();
      if (a->decRefCount() == 0) {
        a->release();
      }
      base->m_data.parr = newData;
    }
    break;
  }
  case KindOfObject: {
    objOffsetSet(base, tvAsCVarRef(key), (TypedValue*)value);
    break;
  }
  default: ASSERT(false);
  }
}

static inline void SetNewElemEmptyish(TypedValue* base, Cell* value) {
  Array a = Array::Create();
  a.append(tvCellAsCVarRef(value));
  tvAsVariant(base) = a;
}
static inline void SetNewElemNumberish(Cell* value) {
  raise_warning(Strings::CANNOT_USE_SCALAR_AS_ARRAY);
  tvRefcountedDecRefCell((TypedValue*)value);
  tvWriteNull((TypedValue*)value);
}
static inline void SetNewElem(TypedValue* base, Cell* value) {
  DataType type;
  opPre(base, type);
  switch (type) {
  case KindOfUninit:
  case KindOfNull: {
    SetNewElemEmptyish(base, value);
    break;
  }
  case KindOfBoolean: {
    if (base->m_data.num) {
      SetNewElemNumberish(value);
    } else {
      SetNewElemEmptyish(base, value);
    }
    break;
  }
  case KindOfInt32:
  case KindOfInt64:
  case KindOfDouble: {
    SetNewElemNumberish(value);
    break;
  }
  case KindOfStaticString:
  case KindOfString: {
    int baseLen = base->m_data.pstr->size();
    if (baseLen == 0) {
      SetNewElemEmptyish(base, value);
    } else {
      raise_error("[] operator not supported for strings");
    }
    break;
  }
  case KindOfArray: {
    ArrayData* a = base->m_data.parr;
    bool copy = (a->getCount() > 1)
                || (value->m_type == KindOfArray && value->m_data.parr == a);
    a = a->append(tvCellAsCVarRef(value), copy);
    if (a) {
      a->incRefCount();
      base->m_data.parr->decRefCount();
      base->m_data.parr = a;
    }
    break;
  }
  case KindOfObject: {
    objOffsetSet(base, init_null_variant, (TypedValue*)value);
    break;
  }
  default: ASSERT(false);
  }
}

static inline TypedValue* SetOpElemEmptyish(unsigned char op, TypedValue* base,
                                            TypedValue* key, Cell* rhs) {
  Array a = Array::Create();
  TypedValue* result = (TypedValue*)&a.lvalAt(tvAsCVarRef(key));
  tvAsVariant(base) = a;
  if (MoreWarnings) {
    raise_notice(Strings::UNDEFINED_INDEX,
                 tvAsCVarRef(key).toString().data());
  }
  SETOP_BODY(result, op, rhs);
  return result;
}
static inline TypedValue* SetOpElemNumberish(TypedValue& tvScratch) {
  raise_warning(Strings::CANNOT_USE_SCALAR_AS_ARRAY);
  tvWriteNull(&tvScratch);
  return &tvScratch;
}
static inline TypedValue* SetOpElem(TypedValue& tvScratch, TypedValue& tvRef,
                                    unsigned char op, TypedValue* base,
                                    TypedValue* key, Cell* rhs) {
  TypedValue* result;
  DataType type;
  opPre(base, type);
  switch (type) {
  case KindOfUninit:
  case KindOfNull: {
    result = SetOpElemEmptyish(op, base, key, rhs);
    break;
  }
  case KindOfBoolean: {
    if (base->m_data.num) {
      result = SetOpElemNumberish(tvScratch);
    } else {
      result = SetOpElemEmptyish(op, base, key, rhs);
    }
    break;
  }
  case KindOfInt32:
  case KindOfInt64:
  case KindOfDouble: {
    result = SetOpElemNumberish(tvScratch);
    break;
  }
  case KindOfStaticString:
  case KindOfString: {
    if (base->m_data.pstr->size() != 0) {
      raise_error("Invalid SetOpElem operand");
    }
    result = SetOpElemEmptyish(op, base, key, rhs);
    break;
  }
  case KindOfArray: {
    result = ElemDArray<MoreWarnings>(base, key);
    SETOP_BODY(result, op, rhs);
    break;
  }
  case KindOfObject: {
    result = objOffsetGet(tvRef, base, tvAsCVarRef(key));
    SETOP_BODY(result, op, rhs);
    objOffsetSet(base, tvAsCVarRef(key), result, false);
    break;
  }
  default: {
    ASSERT(false);
    result = NULL; // Silence compiler warning.
  }
  }
  return result;
}

static inline TypedValue* SetOpNewElemEmptyish(unsigned char op,
                                               TypedValue* base, Cell* rhs) {
  Array a = Array::Create();
  TypedValue* result = (TypedValue*)&a.lvalAt();
  tvAsVariant(base) = a;
  SETOP_BODY(result, op, rhs);
  return result;
}
static inline TypedValue* SetOpNewElemNumberish(TypedValue& tvScratch) {
  raise_warning(Strings::CANNOT_USE_SCALAR_AS_ARRAY);
  tvWriteNull(&tvScratch);
  return &tvScratch;
}
static inline TypedValue* SetOpNewElem(TypedValue& tvScratch, TypedValue& tvRef,
                                       unsigned char op, TypedValue* base,
                                       Cell* rhs) {
  TypedValue* result;
  DataType type;
  opPre(base, type);
  switch (type) {
  case KindOfUninit:
  case KindOfNull: {
    result = SetOpNewElemEmptyish(op, base, rhs);
    break;
  }
  case KindOfBoolean: {
    if (base->m_data.num) {
      result = SetOpNewElemNumberish(tvScratch);
    } else {
      result = SetOpNewElemEmptyish(op, base, rhs);
    }
    break;
  }
  case KindOfInt32:
  case KindOfInt64:
  case KindOfDouble: {
    result = SetOpNewElemNumberish(tvScratch);
    break;
  }
  case KindOfStaticString:
  case KindOfString: {
    if (base->m_data.pstr->size() != 0) {
      raise_error("[] operator not supported for strings");
    }
    result = SetOpNewElemEmptyish(op, base, rhs);
    break;
  }
  case KindOfArray: {
    result = (TypedValue*)&tvAsVariant(base).asArrRef().lvalAt();
    SETOP_BODY(result, op, rhs);
    break;
  }
  case KindOfObject: {
    result = objOffsetGet(tvRef, base, init_null_variant);
    SETOP_BODY(result, op, rhs);
    objOffsetSet(base, init_null_variant, result, false);
    break;
  }
  default: {
    ASSERT(false);
    result = NULL; // Silence compiler warning.
  }
  }
  return result;
}

static inline void IncDecElemEmptyish(unsigned char op, TypedValue* base,
                                      TypedValue* key, TypedValue& dest) {
  Array a = Array::Create();
  TypedValue* result = (TypedValue*)&a.lvalAt(tvAsCVarRef(key));
  tvAsVariant(base) = a;
  if (MoreWarnings) {
    raise_notice(Strings::UNDEFINED_INDEX,
                 tvAsCVarRef(key).toString().data());
  }
  IncDecBody(op, result, &dest);
}
static inline void IncDecElemNumberish(TypedValue& dest) {
  raise_warning(Strings::CANNOT_USE_SCALAR_AS_ARRAY);
  tvWriteNull(&dest);
}
static inline void IncDecElem(TypedValue& tvScratch, TypedValue& tvRef,
                              unsigned char op, TypedValue* base,
                              TypedValue* key, TypedValue& dest) {
  DataType type;
  opPre(base, type);
  switch (type) {
  case KindOfUninit:
  case KindOfNull: {
    IncDecElemEmptyish(op, base, key, dest);
    break;
  }
  case KindOfBoolean: {
    if (base->m_data.num) {
      IncDecElemNumberish(dest);
    } else {
      IncDecElemEmptyish(op, base, key, dest);
    }
    break;
  }
  case KindOfInt32:
  case KindOfInt64:
  case KindOfDouble: {
    IncDecElemNumberish(dest);
    break;
  }
  case KindOfStaticString:
  case KindOfString: {
    if (base->m_data.pstr->size() != 0) {
      raise_error("Invalid IncDecElem operand");
    }
    IncDecElemEmptyish(op, base, key, dest);
    break;
  }
  case KindOfArray: {
    TypedValue* result = ElemDArray<MoreWarnings>(base, key);
    IncDecBody(op, result, &dest);
    break;
  }
  case KindOfObject: {
    TypedValue* result = objOffsetGet(tvRef, base, tvAsCVarRef(key));
    IncDecBody(op, result, &dest);
    break;
  }
  default: ASSERT(false);
  }
}

static inline void IncDecNewElemEmptyish(unsigned char op, TypedValue* base,
                                         TypedValue& dest) {
  Array a = Array::Create();
  TypedValue* result = (TypedValue*)&a.lvalAt();
  tvAsVariant(base) = a;
  IncDecBody(op, result, &dest);
}
static inline void IncDecNewElemNumberish(TypedValue& dest) {
  raise_warning(Strings::CANNOT_USE_SCALAR_AS_ARRAY);
  tvWriteNull(&dest);
}
static inline void IncDecNewElem(TypedValue& tvScratch, TypedValue& tvRef,
                                 unsigned char op, TypedValue* base,
                                 TypedValue& dest) {
  DataType type;
  opPre(base, type);
  switch (type) {
  case KindOfUninit:
  case KindOfNull: {
    IncDecNewElemEmptyish(op, base, dest);
    break;
  }
  case KindOfBoolean: {
    if (base->m_data.num) {
      IncDecNewElemNumberish(dest);
    } else {
      IncDecNewElemEmptyish(op, base, dest);
    }
    break;
  }
  case KindOfInt32:
  case KindOfInt64:
  case KindOfDouble: {
    IncDecNewElemNumberish(dest);
    break;
  }
  case KindOfStaticString:
  case KindOfString: {
    if (base->m_data.pstr->size() != 0) {
      raise_error("Invalid IncDecNewElem operand");
    }
    IncDecNewElemEmptyish(op, base, dest);
    break;
  }
  case KindOfArray: {
    TypedValue* result = (TypedValue*)&tvAsVariant(base).asArrRef().lvalAt();
    IncDecBody(op, result, &dest);
    break;
  }
  case KindOfObject: {
    TypedValue* result = objOffsetGet(tvRef, base,
                                      init_null_variant);
    IncDecBody(op, result, &dest);
    break;
  }
  default: ASSERT(false);
  }
}

void IncDecBody(unsigned char op, TypedValue* fr, TypedValue* to) {
  if (fr->m_type == KindOfInt64) {
    switch ((IncDecOp)op) {
    case PreInc: {
      ++(fr->m_data.num);
      tvDupCell(fr, to);
      break;
    }
    case PostInc: {
      tvDupCell(fr, to);
      ++(fr->m_data.num);
      break;
    }
    case PreDec: {
      --(fr->m_data.num);
      tvDupCell(fr, to);
      break;
    }
    case PostDec: {
      tvDupCell(fr, to);
      --(fr->m_data.num);
      break;
    }
    default: ASSERT(false);
    }
    return;
  }
  if (fr->m_type == KindOfUninit) {
    ActRec* fp = g_vmContext->m_fp;
    size_t pind = ((uintptr_t(fp) - uintptr_t(fr)) / sizeof(TypedValue)) - 1;
    if (pind < fp->m_func->pnames().size()) {
      // Only raise a warning if fr points to a local variable
      raise_notice(Strings::UNDEFINED_VARIABLE,
                   fp->m_func->pnames()[pind]->data());
    }
    // Convert uninit null to null so that we don't write out an uninit null
    // to the eval stack for PostInc and PostDec.
    fr->m_type = KindOfNull;
  }
  switch ((IncDecOp)op) {
  case PreInc: {
    ++(tvAsVariant(fr));
    tvReadCell(fr, to);
    break;
  }
  case PostInc: {
    tvReadCell(fr, to);
    ++(tvAsVariant(fr));
    break;
  }
  case PreDec: {
    --(tvAsVariant(fr));
    tvReadCell(fr, to);
    break;
  }
  case PostDec: {
    tvReadCell(fr, to);
    --(tvAsVariant(fr));
    break;
  }
  default: ASSERT(false);
  }
}

static inline void UnsetElem(TypedValue* base, TypedValue* member) {
  DataType type;
  opPre(base, type);
  switch (type) {
  case KindOfStaticString:
  case KindOfString: {
    raise_error("Cannot unset string offsets");
  }
  case KindOfArray: {
    ArrayData* a = base->m_data.parr;
    bool copy = (a->getCount() > 1);
    int64 n;
    if (IS_STRING_TYPE(member->m_type)) {
      if (member->m_data.pstr->isStrictlyInteger(n)) {
        a = a->remove(n, copy);
      } else {
        a = a->remove(tvAsCVarRef(member), copy);
      }
    } else if (member->m_type == KindOfInt32 || member->m_type == KindOfInt64) {
      a = a->remove(member->m_data.num, copy);
    } else {
      CVarRef memberCVR = tvAsCVarRef(member);
      const VarNR &key = memberCVR.toKey();
      if (key.isNull()) {
        return;
      }
      a = a->remove(key, copy);
    }
    if (a) {
      a->incRefCount();
      base->m_data.parr->decRefCount();
      base->m_data.parr = a;
    }
    break;
  }
  case KindOfObject: {
    objOffsetUnset(base, tvAsCVarRef(member));
    break;
  }
  default: break; // Do nothing.
  }
}

template <bool warn>
static inline DataType propPreNull(TypedValue& tvScratch, TypedValue*& result) {
  TV_WRITE_NULL(&tvScratch);
  result = &tvScratch;
  if (warn) {
    raise_warning("Cannot access property on non-object");
  }
  return KindOfNull;
}
template <bool warn, bool define>
static inline DataType propPreStdclass(TypedValue& tvScratch,
                                       TypedValue*& result, TypedValue* base,
                                       TypedValue* key, StringData*& keySD) {
  if (!define) {
    return propPreNull<warn>(tvScratch, result);
  }
  Instance* obj = newInstance(SystemLib::s_stdclassClass);
  tvRefcountedDecRef(base);
  base->m_type = KindOfObject;
  base->m_data.pobj = obj;
  obj->incRefCount();
  result = base;
  if (warn) {
    raise_warning("Cannot access property on non-object");
  }
  keySD = prepareKey(key).detach();
  return KindOfObject;
}

template <bool warn, bool define, bool issetEmpty>
static inline DataType propPre(TypedValue& tvScratch, TypedValue*& result,
                               TypedValue*& base, TypedValue* key,
                               StringData*& keySD) {
  DataType type;
  opPre(base, type);
  switch (type) {
  case KindOfUninit:
  case KindOfNull: {
    return propPreStdclass<warn, define>(tvScratch, result, base, key, keySD);
  }
  case KindOfBoolean: {
    if (base->m_data.num) {
      return propPreNull<warn>(tvScratch, result);
    } else {
      return propPreStdclass<warn, define>(tvScratch, result, base, key, keySD);
    }
  }
  case KindOfStaticString:
  case KindOfString: {
    if (base->m_data.pstr->size() != 0) {
      return propPreNull<warn>(tvScratch, result);
    } else {
      return propPreStdclass<warn, define>(tvScratch, result, base, key, keySD);
    }
  }
  case KindOfArray: {
    return issetEmpty ? KindOfArray : propPreNull<warn>(tvScratch, result);
  }
  case KindOfObject: {
    keySD = prepareKey(key).detach();
    return KindOfObject;
  }
  default: {
    return propPreNull<warn>(tvScratch, result);
  }
  }
}

static inline void propPost(StringData* keySD) {
  LITSTR_DECREF(keySD);
}

// define == false:
//   $result = $base->$key;
//
// define == true:
//   $base->$key = ...
//   \____ ____/
//        v
//     $result
template <bool warn, bool define, bool unset>
static inline TypedValue* prop(TypedValue& tvScratch, TypedValue& tvRef,
                               Class* ctx, TypedValue* base, TypedValue* key) {
  ASSERT(!warn || !unset);
  TypedValue* result = NULL;
  StringData* keySD;
  DataType t = propPre<warn, define, false>(tvScratch, result, base, key,
                                            keySD);
  if (t == KindOfNull) {
    return result;
  }
  ASSERT(t == KindOfObject);
  // Get property.
  if (LIKELY(base->m_data.pobj->isInstance())) {
    Instance* instance = static_cast<Instance*>(base->m_data.pobj);
    result = &tvScratch;
#define ARGS result, tvRef, ctx, keySD
    if (!warn && !(define || unset)) instance->prop  (ARGS);
    if (!warn &&  (define || unset)) instance->propD (ARGS);
    if ( warn && !define           ) instance->propW (ARGS);
    if ( warn &&  define           ) instance->propWD(ARGS);
#undef ARGS
  } else {
    // Extension class instance.
    TV_WRITE_UNINIT(&tvRef);
    CStrRef ctxName = ctx ? ctx->nameRef() : null_string;
    if (define || unset) {
      result = (TypedValue*)&base->m_data.pobj->o_lval(StrNR(keySD),
                                                       tvAsCVarRef(&tvRef),
                                                       ctxName);
    } else {
      tvAsVariant(&tvRef) = base->m_data.pobj->o_get(StrNR(keySD),
                                                     warn, /* error */
                                                     ctxName);
      result = &tvRef;
    }
  }
  propPost(keySD);
  return result;
}

template <bool useEmpty>
static inline bool IssetEmptyElem(TypedValue& tvScratch, TypedValue& tvRef,
                                  TypedValue* base, bool baseStrOff,
                                  TypedValue* key) {
  TypedValue* result;
  DataType type;
  opPre(base, type);
  switch (type) {
  case KindOfStaticString:
  case KindOfString: {
    if (baseStrOff) {
      return useEmpty;
    }
    TypedValue tv;
    tvDup(key, &tv);
    tvCastToInt64InPlace(&tv);
    int64 x = tv.m_data.num;
    if (x < 0 || x >= base->m_data.pstr->size()) {
      return useEmpty;
    }
    if (!useEmpty) {
      return true;
    }
    tvAsVariant(&tvScratch) = base->m_data.pstr->getChar(x);
    result = &tvScratch;
    break;
  }
  case KindOfArray: {
    result = ElemArray<false>(base, key);
    break;
  }
  case KindOfObject: {
    if (!useEmpty) {
      return objOffsetExists(base, tvAsCVarRef(key));
    }
    if (!objOffsetExists(base, tvAsCVarRef(key))) {
      return true;
    }
    result = objOffsetGet(tvRef, base, tvAsCVarRef(key), false);
    break;
  }
  default: {
    return useEmpty;
  }
  }

  if (useEmpty) {
    return empty(tvAsCVarRef(result));
  } else {
    return isset(tvAsCVarRef(result));
  }
}

template <bool useEmpty>
static inline bool IssetEmptyProp(Class* ctx, TypedValue* base,
                                  TypedValue* key) {
  StringData* keySD;
  TypedValue tvScratch;
  TypedValue* result = NULL;
  DataType t = propPre<false, false, true>(tvScratch, result, base, key,
                                           keySD);
  if (t == KindOfNull) {
    return useEmpty;
  }
  if (t == KindOfObject) {
    bool issetEmptyResult;
    if (LIKELY(base->m_data.pobj->isInstance())) {
      Instance* instance = static_cast<Instance*>(base->m_data.pobj);
      issetEmptyResult = useEmpty ?
                    instance->propEmpty(ctx, keySD) :
                    instance->propIsset(ctx, keySD);
    } else {
      // Extension class instance.
      CStrRef ctxName = ctx ? ctx->nameRef() : null_string;
      issetEmptyResult = useEmpty ?
                    base->m_data.pobj->o_empty(StrNR(keySD), ctxName) :
                    base->m_data.pobj->o_isset(StrNR(keySD), ctxName);
    }
    propPost(keySD);
    return issetEmptyResult;
  } else {
    ASSERT(t == KindOfArray);
    return useEmpty;
  }
}

static inline void SetPropNull(Cell* val) {
  tvRefcountedDecRefCell(val);
  tvWriteNull(val);
  raise_warning("Cannot access property on non-object");
}
static inline void SetPropStdclass(TypedValue* base, TypedValue* key,
                                   Cell* val) {
  Instance* obj = newInstance(SystemLib::s_stdclassClass);
  obj->incRefCount();
  StringData* keySD = prepareKey(key).detach();
  obj->setProp(NULL, keySD, (TypedValue*)val);
  if (keySD->decRefCount() == 0) {
    keySD->release();
  }
  tvRefcountedDecRef(base);
  base->m_type = KindOfObject;
  base->_count = 0;
  base->m_data.pobj = obj;
}
// $base->$key = $val
static inline void SetProp(Class* ctx, TypedValue* base, TypedValue* key,
                           Cell* val) {
  DataType type;
  opPre(base, type);
  switch (type) {
  case KindOfUninit:
  case KindOfNull: {
    SetPropStdclass(base, key, val);
    break;
  }
  case KindOfBoolean: {
    if (base->m_data.num) {
      SetPropNull(val);
    } else {
      SetPropStdclass(base, key, val);
    }
    break;
  }
  case KindOfStaticString:
  case KindOfString: {
    if (base->m_data.pstr->size() != 0) {
      SetPropNull(val);
    } else {
      SetPropStdclass(base, key, val);
    }
    break;
  }
  case KindOfObject: {
    StringData* keySD = prepareKey(key).detach();
    // Set property.
    if (LIKELY(base->m_data.pobj->isInstance())) {
      Instance* instance = static_cast<Instance*>(base->m_data.pobj);
      instance->setProp(ctx, keySD, val);
    } else {
      // Extension class instance.
      base->m_data.pobj->o_set(keySD, tvCellAsCVarRef(val));
    }
    LITSTR_DECREF(keySD);
    break;
  }
  default: {
    SetPropNull(val);
    break;
  }
  }
}

static inline TypedValue* SetOpPropNull(TypedValue& tvScratch) {
  raise_warning("Attempt to assign property of non-object");
  tvWriteNull(&tvScratch);
  return &tvScratch;
}
static inline TypedValue* SetOpPropStdclass(TypedValue& tvRef, unsigned char op,
                                            TypedValue* base, TypedValue* key,
                                            Cell* rhs) {
  Instance* obj = newInstance(SystemLib::s_stdclassClass);
  obj->incRefCount();
  tvRefcountedDecRef(base);
  base->m_type = KindOfObject;
  base->_count = 0;
  base->m_data.pobj = obj;

  StringData* keySD = prepareKey(key).detach();
  tvWriteNull(&tvRef);
  SETOP_BODY(&tvRef, op, rhs);
  obj->setProp(NULL, keySD, &tvRef);
  LITSTR_DECREF(keySD);
  return &tvRef;
}
// $base->$key <op>= $rhs
static inline TypedValue* SetOpProp(TypedValue& tvScratch, TypedValue& tvRef,
                                    Class* ctx, unsigned char op,
                                    TypedValue* base, TypedValue* key,
                                    Cell* rhs) {
  TypedValue* result;
  DataType type;
  opPre(base, type);
  switch (type) {
  case KindOfUninit:
  case KindOfNull: {
    result = SetOpPropStdclass(tvRef, op, base, key, rhs);
    break;
  }
  case KindOfBoolean: {
    if (base->m_data.num) {
      result = SetOpPropNull(tvScratch);
    } else {
      result = SetOpPropStdclass(tvRef, op, base, key, rhs);
    }
    break;
  }
  case KindOfStaticString:
  case KindOfString: {
    if (base->m_data.pstr->size() != 0) {
      result = SetOpPropNull(tvScratch);
    } else {
      result = SetOpPropStdclass(tvRef, op, base, key, rhs);
    }
    break;
  }
  case KindOfObject: {
    if (LIKELY(base->m_data.pobj->isInstance())) {
      StringData* keySD = prepareKey(key).detach();
      Instance* instance = static_cast<Instance*>(base->m_data.pobj);
      result = instance->setOpProp(tvRef, ctx, op, keySD, rhs);
      LITSTR_DECREF(keySD);
    } else {
      // Extension class instance.
      // XXX Not entirely spec-compliant.
      result = prop<true, false, false>(tvScratch, tvRef, ctx, base, key);
      SETOP_BODY(result, op, rhs);
    }
    break;
  }
  default: {
    result = SetOpPropNull(tvScratch);
    break;
  }
  }
  return result;
}

static inline void IncDecPropNull(TypedValue& dest) {
  raise_warning("Attempt to increment/decrement property of non-object");
  tvWriteNull(&dest);
}
static inline void IncDecPropStdclass(unsigned char op, TypedValue* base,
                                      TypedValue* key, TypedValue& dest) {
  Instance* obj = newInstance(SystemLib::s_stdclassClass);
  obj->incRefCount();
  tvRefcountedDecRef(base);
  base->m_type = KindOfObject;
  base->_count = 0;
  base->m_data.pobj = obj;

  StringData* keySD = prepareKey(key).detach();
  TypedValue tv;
  tvWriteNull(&tv);
  IncDecBody(op, (&tv), &dest);
  obj->setProp(NULL, keySD, &dest);
  ASSERT(!IS_REFCOUNTED_TYPE(tv.m_type));
  LITSTR_DECREF(keySD);
}
static inline void IncDecProp(TypedValue& tvScratch, TypedValue& tvRef,
                              Class* ctx, unsigned char op,
                              TypedValue* base, TypedValue* key,
                              TypedValue& dest) {
  DataType type;
  opPre(base, type);
  switch (type) {
  case KindOfUninit:
  case KindOfNull: {
    IncDecPropStdclass(op, base, key, dest);
    break;
  }
  case KindOfBoolean: {
    if (base->m_data.num) {
      IncDecPropNull(dest);
    } else {
      IncDecPropStdclass(op, base, key, dest);
    }
    break;
  }
  case KindOfStaticString:
  case KindOfString: {
    if (base->m_data.pstr->size() != 0) {
      IncDecPropNull(dest);
    } else {
      IncDecPropStdclass(op, base, key, dest);
    }
    break;
  }
  case KindOfObject: {
    if (LIKELY(base->m_data.pobj->isInstance())) {
      StringData* keySD = prepareKey(key).detach();
      Instance* instance = static_cast<Instance*>(base->m_data.pobj);
      instance->incDecProp(tvRef, ctx, op, keySD, dest);
      LITSTR_DECREF(keySD);
    } else {
      // Extension class instance.
      // XXX Not entirely spec-compliant.
      TypedValue* result = prop<true, true, false>(tvScratch, tvRef,
                                                   ctx, base, key);
      IncDecBody(op, result, &dest);
    }
    break;
  }
  default: {
    IncDecPropNull(dest);
    break;
  }
  }
}

static inline void UnsetProp(Class* ctx, TypedValue* base,
                             TypedValue* key) {
  DataType type;
  opPre(base, type);
  // Validate base.
  if (UNLIKELY(type != KindOfObject)) {
    // Do nothing.
    return;
  }
  // Prepare key.
  StringData* keySD = prepareKey(key).detach();
  // Unset property.
  if (LIKELY(base->m_data.pobj->isInstance())) {
    Instance* instance = static_cast<Instance*>(base->m_data.pobj);
    instance->unsetProp(ctx, keySD);
  } else {
    // Extension class instance.
    base->m_data.pobj->o_unset(keySD);
  }

  LITSTR_DECREF(keySD);
}

static inline Class* frameStaticClass(ActRec* fp) {
  if (fp->hasThis()) {
    return fp->getThis()->getVMClass();
  } else if (fp->hasClass()) {
    return fp->getClass();
  } else {
    not_reached();
  }
}

//=============================================================================
// VarEnv.

VarEnv::VarEnv()
  : m_cfp(NULL)
  , m_name2info(NULL)
  , m_extraArgs(NULL)
  , m_numExtraArgs(0)
  , m_depth(0)
  , m_isGlobalScope(true)
{}

VarEnv::VarEnv(ActRec* fp)
  : m_cfp(fp)
  , m_extraArgs(0)
  , m_numExtraArgs(0)
  , m_depth(1)
  , m_isGlobalScope(false)
{
  const Func* func = fp->m_func;
  const size_t numNames = func->pnames().size();

  m_name2info = NEW(HphpArray)(numNames);
  m_name2info->incRefCount();

  TypedValue** origLocs =
    reinterpret_cast<TypedValue**>(uintptr_t(this) + sizeof(VarEnv));
  TypedValue* loc = frame_local(fp, 0);
  for (unsigned i = 0; i < numNames; ++i, --loc) {
    ASSERT(func->lookupVarId(func->pnames()[i]) == (int)i);
    origLocs[i] = m_name2info->migrateAndSet(
      const_cast<StringData*>(func->pnames()[i]), loc);
  }
}

VarEnv::~VarEnv() {
  TRACE(3, "Destroying VarEnv %p [%s]\n",
           this,
           m_isGlobalScope ? "global scope" : "local scope");
  ASSERT(g_vmContext->m_varEnvs.back() == this);
  ASSERT(m_restoreLocations.empty());
  g_vmContext->m_varEnvs.pop_back();

  ASSERT(m_cfp == NULL);
  if (m_extraArgs != NULL) {
    for (unsigned i = 0; i < m_numExtraArgs; i++) {
      tvRefcountedDecRef(&m_extraArgs[i]);
    }
    free(m_extraArgs);
  }
  if (m_name2info != NULL) {
    if (!m_isGlobalScope) {
      if (m_name2info->decRefCount() == 0) {
        m_name2info->release();
      }
    }
  }
}

VarEnv* VarEnv::createLazyAttach(ActRec* fp,
                                 bool skipInsert /* = false */) {
  const Func* func = fp->m_func;
  const size_t numNames = func->pnames().size();

  void* mem = malloc(sizeof(VarEnv) + sizeof(TypedValue*) * numNames);
  VarEnv* ret = new (mem) VarEnv(fp);
  TRACE(3, "Creating lazily attached VarEnv %p\n", mem);
  if (!skipInsert) {
    g_vmContext->m_varEnvs.push_back(ret);
  }
  return ret;
}

VarEnv* VarEnv::createGlobal() {
  void* mem = malloc(sizeof(VarEnv));
  VarEnv* ret = new (mem) VarEnv();
  TRACE(3, "Creating VarEnv %p [global scope]\n", mem);
  g_vmContext->m_varEnvs.push_back(ret);
  return ret;
}

void VarEnv::destroy(VarEnv* ve) {
  ve->~VarEnv();
  free(ve);
}

void VarEnv::attach(ActRec* fp) {
  TRACE(3, "Attaching VarEnv %p [%s] %d pnames @%p\n",
           this,
           m_isGlobalScope ? "global scope" : "local scope",
           int(fp->m_func->pnames().size()), fp);
  ASSERT(m_depth == 0 || g_vmContext->arGetSfp(fp) == m_cfp ||
         (g_vmContext->arGetSfp(fp) == fp && g_vmContext->isNested()));
  m_cfp = fp;
  m_depth++;
  if (m_name2info == NULL) {
    if (!m_isGlobalScope) {
      m_name2info = NEW(HphpArray)(fp->m_func->pnames().size());
      m_name2info->incRefCount();
    } else {
      SystemGlobals *g = (SystemGlobals*)get_global_variables();
      m_name2info = dynamic_cast<HphpArray*>(
          g->hg_global_storage.getArrayData());
      ASSERT(m_name2info != NULL);
    }
  }
  // Overlay fp's locals.
  const Func* func = fp->m_func;
  TypedValue* loc = frame_local(fp, 0);
  TypedValue** origLocs =
      (TypedValue**)malloc(func->pnames().size() * sizeof(TypedValue*));
  for (unsigned i = 0; i < func->pnames().size(); i++, loc--) {
    origLocs[i] = m_name2info->migrate(
      const_cast<StringData*>(func->pnames()[i]), loc);
  }
  m_restoreLocations.push_back(origLocs);
}

void VarEnv::detach(ActRec* fp) {
  TRACE(3, "Detaching VarEnv %p [%s] @%p\n",
           this,
           m_isGlobalScope ? "global scope" : "local scope",
           fp);
  ASSERT(fp == m_cfp);
  ASSERT(m_depth > 0);
  ASSERT(m_name2info != NULL);
  ASSERT((!m_isGlobalScope && m_depth == 1) == m_restoreLocations.empty());

  /*
   * In the case of a lazily attached VarEnv, we have our locations
   * for the first (lazy) attach stored immediately following the
   * VarEnv in memory.  In this case m_restoreLocations will be empty.
   */
  TypedValue** origLocs =
    !m_restoreLocations.empty()
      ? m_restoreLocations.back()
      : reinterpret_cast<TypedValue**>(uintptr_t(this) + sizeof(VarEnv));

  // Merge/remove fp's overlaid locals.
  const Func* func = fp->m_func;
  for (unsigned i = 0; i < func->pnames().size(); i++) {
    m_name2info->migrate(
      const_cast<StringData*>(func->pnames()[i]), origLocs[i]);
  }
  if (!m_restoreLocations.empty()) {
    m_restoreLocations.pop_back();
    free(origLocs);
  }
  VMExecutionContext* context = g_vmContext;
  m_cfp = context->getPrevVMState(fp);
  m_depth--;
  // don't free global varEnv
  if (m_depth == 0) {
    m_cfp = NULL;
    if (context->m_varEnvs.front() != this) {
      ASSERT(!m_isGlobalScope);
      destroy(this);
    }
  }
}

void VarEnv::set(const StringData* name, TypedValue* tv) {
  ASSERT(m_name2info != NULL);
  m_name2info->nvSet(const_cast<StringData*>(name), tv, false);
}

void VarEnv::bind(const StringData* name, TypedValue* tv) {
  ASSERT(m_name2info != NULL);
  m_name2info->nvBind(const_cast<StringData*>(name), tv, false);
}

void VarEnv::setWithRef(const StringData* name, TypedValue* tv) {
  if (tv->m_type == KindOfVariant) {
    bind(name, tv);
  } else {
    set(name, tv);
  }
}

TypedValue* VarEnv::lookup(const StringData* name) {
  ASSERT(m_name2info != NULL);
  return m_name2info->nvGet(name);
}

bool VarEnv::unset(const StringData* name) {
  ASSERT(m_name2info != NULL);
  m_name2info->nvRemove(const_cast<StringData*>(name), false);
  return true;
}

void VarEnv::setExtraArgs(TypedValue* args, unsigned nargs) {
  m_extraArgs = args;
  m_numExtraArgs = nargs;
}

void VarEnv::copyExtraArgs(TypedValue* args, unsigned nargs) {
  // The VarEnv takes over ownership of the args; the original copies are
  // discarded from the stack without adjusting reference counts, thus allowing
  // VarEnv to avoid reference count manipulation here.
  m_extraArgs = (TypedValue*)malloc(nargs * sizeof(TypedValue));
  m_numExtraArgs = nargs;

  // The stack grows downward, so the args in memory are "backward"; i.e. the
  // leftmost (in PHP) extra arg is highest in memory.  We just copy them in a
  // blob here, and compensate in getExtraArg().
  memcpy(m_extraArgs, args, nargs * sizeof(TypedValue));
}

unsigned VarEnv::numExtraArgs() const {
  return m_numExtraArgs;
}

TypedValue* VarEnv::getExtraArg(unsigned argInd) const {
  ASSERT(argInd < m_numExtraArgs);
  return &m_extraArgs[m_numExtraArgs - argInd - 1];
}

Array VarEnv::getDefinedVariables() const {
  ASSERT(m_name2info != NULL);
  Array ret = Array::Create();
  for (ArrayIter it(m_name2info); !it.end(); it.next()) {
    CVarRef val = it.secondRef();
    if (!val.isInitialized()) {
      continue;
    }
    Variant name(it.first());
    if (val.isReferenced()) {
      ret.setRef(name, val);
    } else {
      ret.add(name, val);
    }
  }
  return ret;
}

//=============================================================================
// Stack.

// Store actual stack elements array in a thread-local in order to amortize the
// cost of allocation.
class StackElms {
 public:
  StackElms() : m_elms(NULL) {}
  ~StackElms() {
    flush();
  }
  TypedValue* elms() {
    if (m_elms == NULL) {
      // RuntimeOption::EvalVMStackElms-sized and -aligned.
      size_t algnSz = RuntimeOption::EvalVMStackElms * sizeof(TypedValue);
      if (posix_memalign((void**)&m_elms, algnSz, algnSz) != 0) {
        throw std::runtime_error(
          std::string("VM stack initialization failed: ") + strerror(errno));
      }
    }
    return m_elms;
  }
  void flush() {
    if (m_elms != NULL) {
      free(m_elms);
      m_elms = NULL;
    }
  }
 private:
  TypedValue* m_elms;
};
IMPLEMENT_THREAD_LOCAL(StackElms, t_se);

const int Stack::sSurprisePageSize = sysconf(_SC_PAGESIZE);
// We reserve the bottom page of each stack for use as the surprise
// page, so the minimum useful stack size is the next power of two.
const uint Stack::sMinStackElms = 2 * sSurprisePageSize / sizeof(TypedValue);

void Stack::ValidateStackSize() {
  if (RuntimeOption::EvalVMStackElms < sMinStackElms) {
    throw std::runtime_error(str(
      boost::format("VM stack size of 0x%llx is below the minimum of 0x%x")
        % RuntimeOption::EvalVMStackElms
        % sMinStackElms));
  }
  if (!Util::isPowerOfTwo(RuntimeOption::EvalVMStackElms)) {
    throw std::runtime_error(str(
      boost::format("VM stack size of 0x%llx is not a power of 2")
        % RuntimeOption::EvalVMStackElms));
  }
}

Stack::Stack()
  : m_elms(NULL), m_top(NULL), m_base(NULL) {
}

Stack::~Stack() {
  requestExit();
}

void
Stack::protect() {
  if (trustSigSegv) {
    mprotect(m_elms, sizeof(void*), PROT_NONE);
  }
}

void
Stack::unprotect() {
  if (trustSigSegv) {
    mprotect(m_elms, sizeof(void*), PROT_READ | PROT_WRITE);
  }
}

void
Stack::requestInit() {
  m_elms = t_se->elms();
  if (trustSigSegv) {
    RequestInjectionData& data = ThreadInfo::s_threadInfo->m_reqInjectionData;
    Lock l(data.surpriseLock);
    ASSERT(data.surprisePage == NULL);
    data.surprisePage = m_elms;
  }
  // Burn one element of the stack, to satisfy the constraint that
  // valid m_top values always have the same high-order (>
  // log(RuntimeOption::EvalVMStackElms)) bits.
  m_top = m_base = m_elms + RuntimeOption::EvalVMStackElms - 1;

  // Because of the surprise page at the bottom of the stack we lose an
  // additional 256 elements which must be taken into account when checking for
  // overflow.
  UNUSED size_t maxelms =
    RuntimeOption::EvalVMStackElms - sSurprisePageSize / sizeof(TypedValue);
  ASSERT(!wouldOverflow(maxelms - 1));
  ASSERT(wouldOverflow(maxelms));

  // Reset permissions on our stack's surprise page
  unprotect();
}

void
Stack::requestExit() {
  if (m_elms != NULL) {
    if (trustSigSegv) {
      RequestInjectionData& data = ThreadInfo::s_threadInfo->m_reqInjectionData;
      Lock l(data.surpriseLock);
      ASSERT(data.surprisePage == m_elms);
      unprotect();
      data.surprisePage = NULL;
    }
    m_elms = NULL;
  }
}

void
Stack::flush() {
  if (!t_se.isNull()) {
    t_se->flush();
  }
}

void Stack::toStringElm(std::ostream& os, TypedValue* tv, const ActRec* fp)
  const {
  if (tv->_count != 0) {
    os << " ??? _count " << tv->_count << " ";
  }
  if (tv->m_type < MinDataType || tv->m_type > MaxNumDataTypes) {
    os << " ??? type " << tv->m_type << "\n";
    return;
  }
  ASSERT(tv->m_type >= MinDataType && tv->m_type < MaxNumDataTypes);
  if (tv->m_type == KindOfCanary) {
    // This can happen while invoking a destructor. The real datum is
    // sitting in a register.
    os << "??? canary\n";
    return;
  }
  if (IS_REFCOUNTED_TYPE(tv->m_type) && tv->m_data.ptv->_count <= 0) {
    // OK in the invoking frame when running a destructor.
    os << " ??? inner_count " << tv->m_data.ptv->_count << " ";
    return;
  }
  switch (tv->m_type) {
  case KindOfVariant:
    os << "V:(";
    os << "@" << tv->m_data.ptv;
    tv = tv->m_data.ptv;  // Unbox so contents get printed below
    ASSERT(tv->m_type != KindOfVariant);
    toStringElm(os, tv, fp);
    os << ")";
    return;
  case KindOfClass:
    os << "A:";
    break;
  default:
    os << "C:";
    break;
  }
  switch (tv->m_type) {
  case KindOfUninit: {
    os << "Undefined";
    break;
  }
  case KindOfNull: {
    os << "Null";
    break;
  }
  case KindOfBoolean: {
    os << (tv->m_data.num ? "True" : "False");
    break;
  }
  case KindOfInt32:
  case KindOfInt64: {
    os << "0x" << std::hex << tv->m_data.num << std::dec;
    break;
  }
  case KindOfDouble: {
    os << tv->m_data.dbl;
    break;
  }
  case KindOfStaticString:
  case KindOfString: {
    ASSERT(tv->m_data.pstr->getCount() > 0);
    int len = tv->m_data.pstr->size();
    bool truncated = false;
    if (len > 128) {
      len = 128;
      truncated = true;
    }
    os << tv->m_data.pstr << ":\""
       << Util::escapeStringForCPP(tv->m_data.pstr->data(), len)
       << "\"" << (truncated ? "..." : "");
    break;
  }
  case KindOfArray: {
    ASSERT(tv->m_data.parr->getCount() > 0);
    os << tv->m_data.parr << ":Array";
    break;
  }
  case KindOfObject: {
    ASSERT(tv->m_data.ptv->_count > 0);
    os << tv->m_data.pobj << ":Object("
       << tvAsVariant(tv).asObjRef().get()->o_getClassName().get()->data()
       << ")";
    break;
  }
  case KindOfVariant: {
    not_reached();
  }
  case KindOfClass: {
    os << tv->m_data.pcls
       << ":" << tv->m_data.pcls->name()->data();
    break;
  }
  default: {
    os << "?";
    break;
  }
  }
}

void Stack::toStringIter(std::ostream& os, Iter* it) const {
  switch (it->m_itype) {
  case Iter::TypeUndefined: {
    os << "I:Undefined";
    break;
  }
  case Iter::TypeArray: {
    os << "I:Array";
    break;
  }
  case Iter::TypeMutableArray: {
    os << "I:MutableArray";
    break;
  }
  case Iter::TypeIterator: {
    os << "I:Iterator";
    break;
  }
  default: {
    ASSERT(false);
    os << "I:?";
    break;
  }
  }
}

void Stack::toStringFrag(std::ostream& os, const ActRec* fp,
                         const TypedValue* top) const {
  TypedValue* tv;

  // The only way to figure out which stack elements are activation records is
  // to follow the frame chain. However, the goal for each stack frame is to
  // print stack fragments from deepest to shallowest -- a then b in the
  // following example:
  //
  //   {func:foo,soff:51}<C:8> {func:bar} C:8 C:1 {func:biz} C:0
  //                           aaaaaaaaaaaaaaaaaa bbbbbbbbbbbbbb
  //
  // Use depth-first recursion to get the output order correct.

  tv = (TypedValue*)((uintptr_t)fp
                - (uintptr_t)(fp->m_func->numLocals() * sizeof(TypedValue))
                - (uintptr_t)(fp->m_func->numIterators() * sizeof(Iter)));

  for (tv--; (uintptr_t)tv >= (uintptr_t)top; tv--) {
    os << " ";
    toStringElm(os, tv, fp);
  }
}

void Stack::toStringAR(std::ostream& os, const ActRec* fp,
                       const FPIEnt *fe, const TypedValue* top) const {
  ActRec *ar = arAtOffset(fp, -fe->m_fpOff);

  if (fe->m_parentIndex != -1) {
    toStringAR(os, fp, &fp->m_func->fpitab()[fe->m_parentIndex],
      (TypedValue*)&ar[1]);
  } else {
    toStringFrag(os, fp, (TypedValue*)&ar[1]);
  }

  os << " {func:" << ar->m_func->fullName()->data() << "}";
  TypedValue* tv = (TypedValue*)ar;
  for (tv--; (uintptr_t)tv >= (uintptr_t)top; tv--) {
    os << " ";
    toStringElm(os, tv, fp);
  }
}

void Stack::toStringFragAR(std::ostream& os, const ActRec* fp,
                           int offset, const TypedValue* top) const {
  const FPIEnt *fe = fp->m_func->findFPI(offset);
  if (fe != NULL) {
    toStringAR(os, fp, fe, top);
  } else {
    toStringFrag(os, fp, top);
  }
}

void Stack::toStringFrame(std::ostream& os, const ActRec* fp,
                          int offset, const TypedValue* ftop,
                          const string& prefix) const {
  ASSERT(fp);

  // Use depth-first recursion to output the most deeply nested stack frame
  // first.
  {
    Offset prevPc;
    TypedValue* prevStackTop;
    ActRec* prevFp = g_vmContext->getPrevVMState(fp, &prevPc, &prevStackTop);
    if (prevFp != NULL) {
      toStringFrame(os, prevFp, prevPc, prevStackTop, prefix);
    }
  }

  os << prefix;
  const Func* func = fp->m_func;
  ASSERT(func);
  func->validate();
  string funcName;
  if (func->isMethod()) {
    funcName = string(func->preClass()->name()->data()) + "::" +
      string(func->name()->data());
  } else {
    funcName = string(func->name()->data());
  }
  os << "{func:" << funcName
     << ",soff:" << fp->m_soff
     << ",this:0x" << std::hex << (fp->hasThis() ? fp->getThis() : NULL)
     << std::dec << "}";
  TypedValue* tv = (TypedValue*)fp;
  tv--;

  if (func->numLocals() > 0) {
    os << "<";
    int n = func->numLocals();
    for (int i = 0; i < n; i++, tv--) {
      if (i > 0) {
        os << " ";
      }
      toStringElm(os, tv, fp);
    }
    os << ">";
  }

  ASSERT(!func->isBuiltin() || func->numIterators() == 0);
  if (func->numIterators() > 0) {
    os << "|";
    Iter* it = &((Iter*)&tv[1])[-1];
    for (int i = 0; i < func->numIterators(); i++, it--) {
      if (i > 0) {
        os << " ";
      }
      if (func->checkIterScope(offset, i)) {
        toStringIter(os, it);
      } else {
        os << "I:Undefined";
      }
    }
    os << "|";
  }

  toStringFragAR(os, fp, offset, ftop);

  os << std::endl;
}

string Stack::toString(const ActRec* fp, int offset,
                       const string prefix/* = "" */) const {
  std::ostringstream os;
  os << "Stack at " << curUnit()->filepath()->data() << ":" <<
    curUnit()->getLineNumber(curUnit()->offsetOf(Transl::vmpc())) << " func " <<
    curFunc()->fullName()->data() << "\n";

  toStringFrame(os, fp, offset, m_top, prefix);

  return os.str();
}

void Stack::clearEvalStack(ActRec *fp, int32 numLocals) {
}

UnwindStatus Stack::unwindFrag(ActRec* fp, int offset,
                               PC& pc, Fault& f) {
  const Func* func = fp->m_func;
  TypedValue* evalTop = (TypedValue*)((uintptr_t)fp
                  - (uintptr_t)(func->numLocals()) * sizeof(TypedValue)
                  - (uintptr_t)(func->numIterators() * sizeof(Iter)));
  while (m_top < evalTop) {
    popTV();
  }

  const EHEnt *eh = func->findEH(offset);
  while (eh != NULL) {
    if (eh->m_ehtype == EHEnt::EHType_Fault) {
      pc = (uchar*)(func->unit()->entry() + eh->m_fault);
      return UnwindResumeVM;
    } else if (f.m_faultType == Fault::KindOfUserException) {
      ObjectData* obj = f.m_userException;
      std::vector<std::pair<Id, Offset> >::const_iterator it;
      for (it = eh->m_catches.begin(); it != eh->m_catches.end(); it++) {
        Class* cls = func->unit()->lookupClass(it->first);
        if (cls && obj->instanceof(cls)) {
          pc = func->unit()->at(it->second);
          return UnwindResumeVM;
        }
      }
    }
    if (eh->m_parentIndex != -1) {
      eh = &func->ehtab()[eh->m_parentIndex];
    } else {
      break;
    }
  }

  if (fp->isFromFPushCtor()) {
    ASSERT(fp->hasThis());
    fp->getThis()->setNoDestruct();
  }

  frame_free_locals_inl(fp, func->numLocals());
  ndiscard(func->numSlotsInFrame());
  return UnwindPropagate;
}

void Stack::unwindARFrag(ActRec* ar) {
  while (m_top < (TypedValue*)ar) {
    popTV();
  }
}

void Stack::unwindAR(ActRec* fp, int offset, const FPIEnt* fe) {
  while (true) {
    TRACE(1, "unwindAR: function %s, offset %d, pIdx %d\n",
          fp->m_func->name()->data(), offset, fe->m_parentIndex);
    ActRec* ar = arAtOffset(fp, -fe->m_fpOff);
    ASSERT((TypedValue*)ar >= m_top);
    unwindARFrag(ar);

    if (ar->isFromFPushCtor()) {
      ASSERT(ar->hasThis());
      ar->getThis()->setNoDestruct();
    }

    popAR();
    if (fe->m_parentIndex != -1) {
      fe = &fp->m_func->fpitab()[fe->m_parentIndex];
    } else {
      return;
    }
  }
}

UnwindStatus Stack::unwindFrame(ActRec*& fp, int offset, PC& pc, Fault& f) {
  VMExecutionContext* context = g_vmContext;

  while (true) {
    SrcKey sk(fp->m_func, offset);
    SKTRACE(1, sk, "unwindFrame: func %s, offset %d fp %p\n",
            fp->m_func->name()->data(),
            offset, fp);
    const FPIEnt *fe = fp->m_func->findFPI(offset);
    if (fe != NULL) {
      unwindAR(fp, offset, fe);
    }
    if (unwindFrag(fp, offset, pc, f) == UnwindResumeVM) {
      return UnwindResumeVM;
    }
    ActRec *prevFp = context->arGetSfp(fp);
    SKTRACE(1, sk, "unwindFrame: fp %p prevFp %p\n",
            fp, prevFp);
    Offset prevOff = fp->m_soff + prevFp->m_func->base();
    // We don't need to refcount the AR's refcounted members; that was
    // taken care of in frame_free_locals, called from unwindFrag()
    discardAR();
    if (prevFp == fp) {
      break;
    }
    // Keep the pc up to date while unwinding.
    const Func *prevF = prevFp->m_func;
    pc = prevF->unit()->at(prevF->base() + fp->m_soff);
    fp = prevFp;
    offset = prevOff;
  }

  return UnwindPropagate;
}

bool Stack::wouldOverflow(int numCells) const {
  // The funny approach here is to validate the translator's assembly
  // technique. We've aligned and sized the stack so that the high order
  // bits of valid cells are all the same. In the translator, numCells
  // can be hardcoded, and m_top is wired into a register,
  // so the expression requires no loads.
  intptr_t truncatedTop = intptr_t(m_top) / sizeof(TypedValue);
  truncatedTop &= RuntimeOption::EvalVMStackElms - 1;
  intptr_t diff = truncatedTop - numCells -
    sSurprisePageSize / sizeof(TypedValue);
  return diff < 0;
}

///////////////////////////////////////////////////////////////////////////////
} // namespace VM

//=============================================================================
// ExecutionContext.

using namespace HPHP::VM;
using namespace HPHP::MethodLookup;

ActRec* VMExecutionContext::arGetSfp(const ActRec* ar) {
  ActRec* prevFrame = (ActRec*)ar->m_savedRbp;
  if (prevFrame >= m_stack.getStackLowAddress() &&
      prevFrame < m_stack.getStackHighAddress()) {
    ASSERT((char*)prevFrame >= ((char*)ar + sizeof(ActRec)));
    return prevFrame;
  }
  return const_cast<ActRec*>(ar);
}

TypedValue* VMExecutionContext::lookupClsCns(const NamedEntity* ne,
                                             const StringData* cls,
                                             const StringData* cns) {
  Class* class_ = Unit::loadClass(ne, cls);
  if (class_ == NULL) {
    raise_error(Strings::UNKNOWN_CLASS, cls->data());
  }
  TypedValue* clsCns = class_->clsCnsGet(cns);
  if (clsCns == NULL) {
    raise_error("Couldn't find constant %s::%s",
                cls->data(), cns->data());
  }
  return clsCns;
}

// Look up the method specified by methodName from the class specified by cls
// and enforce accessibility. Accessibility checks depend on the relationship
// between the class that first declared the method (baseClass) and the context
// class (ctx).
//
// If there are multiple accessible methods with the specified name declared in
// cls and ancestors of cls, the method from the most derived class will be
// returned, except if we are doing an ObjMethod call ("$obj->foo()") and there
// is an accessible private method, in which case the accessible private method
// will be returned.
//
// Accessibility rules:
//
//   | baseClass/ctx relationship | public | protected | private |
//   +----------------------------+--------+-----------+---------+
//   | anon/unrelated             | yes    | no        | no      |
//   | baseClass == ctx           | yes    | yes       | yes     |
//   | baseClass derived from ctx | yes    | yes       | no      |
//   | ctx derived from baseClass | yes    | yes       | no      |
//   +----------------------------+--------+-----------+---------+

const Func* VMExecutionContext::lookupMethodCtx(const Class* cls,
                                                const StringData* methodName,
                                                Class* ctx,
                                                CallType callType,
                                                bool raise /* = false */) {
  const Func* method;
  if (callType == CtorMethod) {
    ASSERT(methodName == NULL);
    method = cls->getCtor();
  } else {
    ASSERT(callType == ObjMethod || callType == ClsMethod);
    ASSERT(methodName != NULL);
    method = cls->lookupMethod(methodName);
    if (!method) {
      static StringData* sd__construct
        = StringData::GetStaticString("__construct");
      if (UNLIKELY(methodName == sd__construct)) {
        // We were looking up __construct and failed to find it. Fall back
        // to old-style constructor: same as class name.
        method = cls->getCtor();
      } else {
        if (raise) {
          raise_error("Call to undefined method %s::%s from %s%s",
                      cls->name()->data(),
                      methodName->data(),
                      ctx ? "context " : "anonymous context",
                      ctx ? ctx->name()->data() : "");
        }
        return NULL;
      }
    }
  }
  ASSERT(method);
  bool accessible = true;
  // If we found a protected or private method, we need to do some
  // accessibility checks.
  if ((method->attrs() & (AttrProtected|AttrPrivate)) &&
      !g_vmContext->getDebuggerBypassCheck()) {
    Class* baseClass = method->baseCls();
    ASSERT(baseClass);
    // If the context class is the same as the class that first
    // declared this method, then we know we have the right method
    // and we can stop here.
    if (ctx == baseClass) {
      return method;
    }
    // The anonymous context cannot access protected or private methods,
    // so we can fail fast here.
    if (ctx == NULL) {
      if (raise) {
        raise_error("Call to %s method %s::%s from anonymous context",
                    (method->attrs() & AttrPrivate) ? "private" : "protected",
                    cls->name()->data(),
                    method->name()->data());
      }
      return NULL;
    }
    ASSERT(ctx);
    if (method->attrs() & AttrPrivate) {
      // The context class is not the same as the class that declared
      // this private method, so this private method is not accessible.
      // We need to keep going because the context class may define a
      // private method with this name.
      accessible = false;
    } else {
      // If the context class is derived from the class that first
      // declared this protected method, then we know this method is
      // accessible and we know the context class cannot have a private
      // method with the same name, so we're done.
      if (ctx->classof(baseClass)) {
        return method;
      }
      if (!baseClass->classof(ctx)) {
        // The context class is not the same, an ancestor, or a descendent
        // of the class that first declared this protected method, so
        // this method is not accessible. Because the context class is
        // not the same or an ancestor of the class the first declared
        // the method, we know that the context class is not the same
        // or an ancestor of cls, and therefore we don't need to check
        // if the context class declares a private method with this name,
        // so we can fail fast here.
        if (raise) {
          raise_error("Call to protected method %s::%s from context %s",
                      cls->name()->data(),
                      method->name()->data(),
                      ctx->name()->data());
        }
        return NULL;
      }
      // We now know this protected method is accessible, but we need to
      // keep going because the context class may define a private method
      // with this name.
      ASSERT(accessible && baseClass->classof(ctx));
    }
  }
  // If this is an ObjMethod call ("$obj->foo()") AND there is an ancestor
  // of cls that declares a private method with this name AND the context
  // class is an ancestor of cls, check if the context class declares a
  // private method with this name.
  if (method->hasPrivateAncestor() && callType == ObjMethod &&
      ctx && cls->classof(ctx)) {
    const Func* ctxMethod = ctx->lookupMethod(methodName);
    if (ctxMethod && ctxMethod->cls() == ctx &&
        (ctxMethod->attrs() & AttrPrivate)) {
      // For ObjMethod calls a private method from the context class
      // trumps any other method we may have found.
      return ctxMethod;
    }
  }
  if (accessible) {
    return method;
  }
  if (raise) {
    raise_error("Call to private method %s::%s from %s%s",
                method->baseCls()->name()->data(),
                method->name()->data(),
                ctx ? "context " : "anonymous context",
                ctx ? ctx->name()->data() : "");
  }
  return NULL;
}

LookupResult VMExecutionContext::lookupObjMethod(const Func*& f,
                                                 const Class* cls,
                                                 const StringData* methodName,
                                                 bool raise /* = false */) {
  Class* ctx = arGetContextClass(m_fp);
  f = lookupMethodCtx(cls, methodName, ctx, ObjMethod, false);
  if (!f) {
    f = cls->lookupMethod(s___call.get());
    if (!f) {
      if (raise) {
        // Throw a fatal error
        lookupMethodCtx(cls, methodName, ctx, ObjMethod, true);
      }
      return MethodNotFound;
    }
    return MagicCallFound;
  }
  if (f->attrs() & AttrStatic) {
    return MethodFoundNoThis;
  }
  return MethodFoundWithThis;
}

LookupResult
VMExecutionContext::lookupClsMethod(const Func*& f,
                                    const Class* cls,
                                    const StringData* methodName,
                                    ObjectData* obj,
                                    bool raise /* = false */) {
  Class* ctx = arGetContextClass(m_fp);
  f = lookupMethodCtx(cls, methodName, ctx, ClsMethod, false);
  if (!f) {
    if (obj && obj->instanceof(cls)) {
      f = obj->getVMClass()->lookupMethod(s___call.get());
    }
    if (!f) {
      f = cls->lookupMethod(s___callStatic.get());
      if (!f) {
        if (raise) {
          // Throw a fatal errpr
          lookupMethodCtx(cls, methodName, ctx, ClsMethod, true);
        }
        return MethodNotFound;
      }
      f->validate();
      ASSERT(f);
      ASSERT(f->attrs() & AttrStatic);
      return MagicCallStaticFound;
    }
    ASSERT(f);
    ASSERT(obj);
    // __call cannot be static, this should be enforced by semantic
    // checks defClass time or earlier
    ASSERT(!(f->attrs() & AttrStatic));
    return MagicCallFound;
  }
  if (obj && !(f->attrs() & AttrStatic) && obj->instanceof(cls)) {
    return MethodFoundWithThis;
  }
  return MethodFoundNoThis;
}

LookupResult VMExecutionContext::lookupCtorMethod(const Func*& f,
                                                  const Class* cls,
                                                  bool raise /* = false */) {
  Class* ctx = arGetContextClass(m_fp);
  f = lookupMethodCtx(cls, NULL, ctx, CtorMethod, raise);
  if (!f) {
    // If raise was true than lookupMethodCtx should have thrown,
    // so we should only be able to get here if raise was false
    ASSERT(!raise);
    return MethodNotFound;
  }
  return MethodFoundWithThis;
}

ObjectData* VMExecutionContext::createObject(StringData* clsName,
                                             CArrRef params,
                                             bool init /* = true */) {
  Class* class_ = Unit::loadClass(clsName);
  if (class_ == NULL) {
    throw_missing_class(clsName->data());
  }
  Object o;
  o = newInstance(class_);
  if (init) {
    // call constructor
    TypedValue ret;
    invokeFunc(&ret, class_->getCtor(), params, o.get());
    tvRefcountedDecRef(&ret);
  }

  ObjectData* ret = o.detach();
  ret->decRefCount();
  return ret;
}

ObjectData* VMExecutionContext::createObjectOnly(StringData* clsName) {
  return createObject(clsName, null_array, false);
}

ActRec* VMExecutionContext::getStackFrame() {
  VMRegAnchor _;
  return m_fp;
}

ObjectData* VMExecutionContext::getThis(bool skipFrame /* = false */) {
  VMRegAnchor _;
  ActRec* fp = m_fp;
  if (skipFrame) {
    fp = getPrevVMState(fp);
    if (!fp) return NULL;
  }
  if (fp->hasThis()) {
    return fp->getThis();
  }
  return NULL;
}

CStrRef VMExecutionContext::getContextClassName(bool skipFrame /* = false */) {
  VMRegAnchor _;
  ActRec* ar = m_fp;
  ASSERT(ar != NULL);
  if (skipFrame) {
    ar = getPrevVMState(ar);
    if (!ar) return empty_string;
  }
  if (ar->hasThis()) {
    return ar->getThis()->o_getClassName();
  } else if (ar->hasClass()) {
    return ar->getClass()->nameRef();
  } else {
    return empty_string;
  }
}

CStrRef VMExecutionContext::getParentContextClassName(bool skip /* = false */) {
  VMRegAnchor _;
  ActRec* ar = m_fp;
  ASSERT(ar != NULL);
  if (skip) {
    ar = getPrevVMState(ar);
    if (!ar) return empty_string;
  }
  if (ar->hasThis()) {
    const Class* cls = ar->getThis()->getVMClass();
    if (cls->parent() == NULL) {
      return empty_string;
    }
    return cls->parent()->nameRef();
  } else if (ar->hasClass()) {
    const Class* cls = ar->getClass();
    if (cls->parent() == NULL) {
      return empty_string;
    }
    return cls->parent()->nameRef();
  } else {
    return empty_string;
  }
}

CStrRef VMExecutionContext::getContainingFileName(
  bool skipFrame /* = false */) {
  VMRegAnchor _;
  ActRec* ar = m_fp;
  if (ar == NULL) return empty_string;
  if (skipFrame) {
    ar = getPrevVMState(ar);
    if (ar == NULL) return empty_string;
  }
  Unit* unit = ar->m_func->unit();
  return unit->filepathRef();
}

int VMExecutionContext::getLine(bool skipFrame /* = false */) {
  VMRegAnchor _;
  ActRec* ar = m_fp;
  Unit* unit = ar ? ar->m_func->unit() : NULL;
  Offset pc = unit ? pcOff() : 0;
  if (ar == NULL) return -1;
  if (skipFrame) {
    ar = getPrevVMState(ar, &pc);
  }
  if (ar == NULL || (unit = ar->m_func->unit()) == NULL) return -1;
  return unit->getLineNumber(pc);
}

Array VMExecutionContext::getCallerInfo(bool skipFrame /* = false */) {
  VMRegAnchor _;
  Array result = Array::Create();
  ActRec* ar = m_fp;
  if (skipFrame) {
    ar = getPrevVMState(ar);
  }
  while (ar->m_func->name()->isame(s_call_user_func.get())
         || ar->m_func->name()->isame(s_call_user_func_array.get())) {
    ar = getPrevVMState(ar);
    if (ar == NULL) {
      return result;
    }
  }

  Offset pc;
  ar = getPrevVMState(ar, &pc);
  while (ar != NULL) {
    if (!ar->m_func->name()->isame(s_call_user_func.get())
        && !ar->m_func->name()->isame(s_call_user_func_array.get())) {
      Unit* unit = ar->m_func->unit();
      int lineNumber;
      if ((lineNumber = unit->getLineNumber(pc)) != -1) {
        ASSERT(!unit->filepath()->size() ||
               unit->filepath()->data()[0] == '/');
        result.set(s_file, unit->filepath()->data(), true);
        result.set(s_line, lineNumber);
        return result;
      }
    }
    ar = getPrevVMState(ar, &pc);
  }
  return result;
}

bool VMExecutionContext::defined(CStrRef name) {
  return m_constants.nvGet(name.get()) != NULL;
}

TypedValue* VMExecutionContext::getCns(StringData* cns,
                                       bool system /* = true */,
                                       bool dynamic /* = true */) {
  if (dynamic) {
    TypedValue* tv = m_constants.nvGet(cns);
    if (tv != NULL) {
      return tv;
    }
  }
  if (system) {
    const ClassInfo::ConstantInfo* ci = ClassInfo::FindConstant(cns->data());
    if (ci != NULL) {
      if (!dynamic) {
        ConstInfoMap::const_iterator it = m_constInfo.find(cns);
        if (it != m_constInfo.end()) {
          // This is a dynamic constant, so don't report it.
          ASSERT(ci == it->second);
          return NULL;
        }
      }
      TypedValue tv;
      TV_WRITE_UNINIT(&tv);
      tvAsVariant(&tv) = ci->getValue();
      m_constants.nvSet(cns, &tv, false);
      tvRefcountedDecRef(&tv);
      return m_constants.nvGet(cns);
    }
  }
  return NULL;
}

bool VMExecutionContext::setCns(StringData* cns, CVarRef val, bool dynamic) {
  if (m_constants.nvGet(cns) != NULL ||
      ClassInfo::FindConstant(cns->data()) != NULL) {
    raise_warning(Strings::CONSTANT_ALREADY_DEFINED, cns->data());
    return false;
  }
  if (!val.isAllowedAsConstantValue()) {
    raise_warning(Strings::CONSTANTS_MUST_BE_SCALAR);
    return false;
  }
  val.setEvalScalar();
  TypedValue* tv = val.getTypedAccessor();
  m_constants.nvSet(cns, tv, false);
  ASSERT(m_constants.nvGet(cns) != NULL);
  if (RuntimeOption::EvalJit) {
    if (dynamic) {
      newPreConst(cns, *tv);
    }
    m_transl->defineCns(cns);
  }
  return true;
}

void VMExecutionContext::newPreConst(StringData* name,
                                     const TypedValue& val) {
  name->incRefCount();
  PreConst pc = { val, this, name };
  m_preConsts.push_back(pc);
  VM::Transl::mergePreConst(m_preConsts.back());
}

bool VMExecutionContext::renameFunction(const StringData* oldName,
                                        const StringData* newName) {
  return m_renamedFuncs.rename(oldName, newName);
}

bool VMExecutionContext::isFunctionRenameable(const StringData* name) {
  return m_renamedFuncs.isFunctionRenameable(name);
}

void VMExecutionContext::addRenameableFunctions(ArrayData* arr) {
  m_renamedFuncs.addRenameableFunctions(arr);
}

VarEnv* VMExecutionContext::getVarEnv(bool skipBuiltin) {
  Transl::VMRegAnchor _;

  HPHP::VM::VarEnv* builtinVarEnv = NULL;
  HPHP::VM::ActRec* fp = m_fp;
  if (skipBuiltin) {
    builtinVarEnv = fp->m_varEnv;
    fp = getPrevVMState(fp);
  }
  if (!fp) return NULL;
  ASSERT(!fp->hasInvName());
  if (fp->m_varEnv == NULL) {
    if (builtinVarEnv) {
      // If the builtin function has its own VarEnv, we temporarily
      // remove it from the list before making a VarEnv for the calling
      // function to satisfy various ASSERTs
      ASSERT(builtinVarEnv == g_vmContext->m_varEnvs.back());
      g_vmContext->m_varEnvs.pop_back();
    }
    fp->m_varEnv = VarEnv::createLazyAttach(fp);
    if (builtinVarEnv) {
      // Put the builtin function's VarEnv back in the list
      g_vmContext->m_varEnvs.push_back(builtinVarEnv);
    }
  }
  return fp->m_varEnv;
}

void VMExecutionContext::setVar(StringData* name, TypedValue* v, bool ref) {
  Transl::VMRegAnchor _;
  // setVar() should only be called after getVarEnv() has been called
  // to create a varEnv
  ActRec *fp = getPrevVMState(m_fp);
  if (!fp) return;
  ASSERT(!fp->hasInvName());
  ASSERT(fp->m_varEnv != NULL);
  if (ref) {
    fp->m_varEnv->bind(name, v);
  } else {
    fp->m_varEnv->set(name, v);
  }
}

Array VMExecutionContext::getLocalDefinedVariables(int frame) {
  Transl::VMRegAnchor _;
  ActRec *fp = m_fp;
  for (; frame > 0; --frame) {
    if (!fp) break;
    fp = getPrevVMState(fp);
  }
  if (!fp) {
    return Array::Create();
  }
  ASSERT(!fp->hasInvName());
  if (fp->m_varEnv) {
    return fp->m_varEnv->getDefinedVariables();
  }
  Array ret = Array::Create();
  const Func *func = fp->m_func;
  Func::PnameMap::const_iterator it = func->pnameMap().begin();
  for (; it != func->pnameMap().end(); ++it) {
    TypedValue* ptv = frame_local(fp, it->second);
    if (ptv->m_type == KindOfUninit) {
      continue;
    }
    Variant name(it->first->data());
    ret.add(name, tvAsVariant(ptv));
  }
  return ret;
}

void VMExecutionContext::shuffleMagicArgs(ActRec* ar) {
  // We need to put this where the first argument is
  StringData* invName = ar->getInvName();
  int nargs = ar->numArgs();
  ar->setVarEnv(NULL);
  ASSERT(!ar->hasVarEnv() && !ar->hasInvName());
  // We need to make an array containing all the arguments passed by the
  // caller and put it where the second argument is
  HphpArray* argArray = pack_args_into_array(ar, nargs);
  argArray->incRefCount();
  // Remove the arguments from the stack
  for (int i = 0; i < nargs; ++i) {
    m_stack.popC();
  }
  // Move invName to where the first argument belongs, no need
  // to incRef/decRef since we are transferring ownership
  m_stack.pushStringNoRc(invName);
  // Move argArray to where the second argument belongs. We've already
  // incReffed the array above so we don't need to do it here.
  m_stack.pushArrayNoRc(argArray);

  ar->setNumArgs(2);
}

static inline void checkStack(Stack& stk, const Func* f) {
  ThreadInfo* info = ThreadInfo::s_threadInfo.getNoCheck();
  // Check whether func's maximum stack usage would overflow the stack.
  // Both native and VM stack overflows are independently possible.
  if (!stack_in_bounds(info) ||
      stk.wouldOverflow(f->maxStackCells())) {
    TRACE(1, "Maximum VM stack depth exceeded.\n");
    raise_error("Stack overflow");
  }
}

template <bool reenter, bool handle_throw>
bool VMExecutionContext::prepareFuncEntry(ActRec *ar, PC& pc) {
  const Func* func = ar->m_func;
  if (!reenter) {
    // For the reenter case, intercept and magic shuffling are handled
    // in invokeFunc() before calling prepareFuncEntry(), so we only
    // need to perform these checks for the non-reenter case.
    if (UNLIKELY(func->maybeIntercepted())) {
      Variant *h = get_intercept_handler(func->fullNameRef(),
                                         &func->maybeIntercepted());
      if (h && !run_intercept_handler<handle_throw>(ar, h)) {
        return false;
      }
    } 
    if (UNLIKELY(ar->hasInvName())) {
      shuffleMagicArgs(ar);
    }
  }
  // It is now safe to access m_varEnv directly
  ASSERT(!ar->hasInvName());
  int nargs = ar->numArgs();
  // Set pc below, once we know that DV dispatch is unnecessary.
  m_fp = ar;
  if (reenter) {
    m_nestedVMMap[ar] = m_nestedVMs.size() - 1;
  }
  bool raiseMissingArgumentWarnings = false;
  int nparams = func->numParams();
  Offset firstDVInitializer = InvalidAbsoluteOffset;
  if (nargs != nparams) {
    if (nargs < nparams) {
      // Push uninitialized nulls for missing arguments. Some of them may end
      // up getting default-initialized, but regardless, we need to make space
      // for them on the stack.
      const Func::ParamInfoVec& paramInfo = func->params();
      for (int i = nargs; i < nparams; ++i) {
        m_stack.pushUninit();
        Offset dvInitializer = paramInfo[i].funcletOff();
        if (dvInitializer == InvalidAbsoluteOffset) {
          // We wait to raise warnings until after all the locals have been
          // initialized. This is important because things need to be in a
          // consistent state in case the user error handler throws.
          raiseMissingArgumentWarnings = true;
        } else if (firstDVInitializer == InvalidAbsoluteOffset) {
          // This is the first unpassed arg with a default value, so
          // this is where we'll need to jump to.
          firstDVInitializer = dvInitializer;
        }
      }
      ASSERT(m_fp->m_func == func);
    } else {
      // For the reenter case, extra arguments are handled by invokeFunc()
      // and enterVM(), so we should only execute the logic below for the
      // non-reenter case.
      if (!reenter) {
        // If there are extra parameters then we cannot be a pseudomain
        // inheriting a VarEnv
        ASSERT(!m_fp->m_varEnv);
        // Extra parameters must be moved off the stack.
        m_fp->m_varEnv = VarEnv::createLazyAttach(m_fp);
        int numExtras = nargs - nparams;
        m_fp->m_varEnv->copyExtraArgs(
          (TypedValue*)(uintptr_t(m_fp) - nargs * sizeof(TypedValue)),
          numExtras);
        for (int i = 0; i < numExtras; i++) {
          m_stack.discard();
        }
      }
    }
  }
  pushLocalsAndIterators(func, nparams);
  if (raiseMissingArgumentWarnings) {
    // cppext functions/methods have their own logic for raising warnings
    // for missing arguments, so we only need to do this work for non-cppext
    // functions/methods
    if (!func->isBuiltin()) {
      pc = func->getEntry();
      // m_pc is not set to callee. if the callee is in a different unit,
      // debugBacktrace() can barf in unit->offsetOf(m_pc) where it
      // asserts that m_pc >= m_bc && m_pc < m_bc + m_bclen. Sync m_fp
      // to function entry point in called unit.
      SYNC();
      const Func::ParamInfoVec& paramInfo = func->params();
      for (int i = nargs; i < nparams; ++i) {
        Offset dvInitializer = paramInfo[i].funcletOff();
        if (dvInitializer == InvalidAbsoluteOffset) {
          raise_warning("Missing argument %d to %s()",
                        i + 1, func->name()->data());
        }
      }
    }
  }
  if (firstDVInitializer != InvalidAbsoluteOffset) {
    pc = func->unit()->entry() + firstDVInitializer;
  } else {
    pc = func->getEntry();
  }
  return true;
}

void VMExecutionContext::syncGdbState() {
  if (RuntimeOption::EvalJit && !RuntimeOption::EvalJitNoGdb) {
    ((VM::Transl::TranslatorX64*)m_transl)->m_debugInfo.debugSync();
  }
}

void VMExecutionContext::enterVMWork(ActRec* ar, bool enterFn) {
  EXCEPTION_GATE_ENTER();
  if (enterFn) {
    EventHook::FunctionEnter(ar, EventHook::NormalFunc);
    INST_HOOK_FENTRY(ar->m_func->fullName());
  }
  Stats::inc(Stats::VMEnter);
  if (RuntimeOption::EvalJit &&
      !ThreadInfo::s_threadInfo->m_reqInjectionData.coverage &&
      !(RuntimeOption::EvalJitDisabledByHphpd && isDebuggerAttached()) &&
      LIKELY(!DEBUGGER_FORCE_INTR)) {
    Transl::SrcKey sk(Transl::curFunc(), m_pc);
    (void) curUnit()->offsetOf(m_pc); /* assert */
    m_transl->resume(sk);
  } else {
    dispatch();
  }
  EXCEPTION_GATE_RETURN();
}

void VMExecutionContext::enterVM(TypedValue* retval,
                                 ActRec* ar,
                                 TypedValue* extraArgs) {
  // It is the caller's responsibility to perform a stack overflow
  // check if necessary before calling enterVM() or reenterVM()
  bool enter = prepareFuncEntry<true, true>(ar, m_pc);

  if (ar->m_varEnv) {
    // If this is a pseudomain inheriting a VarEnv from our caller,
    // there cannot be extra arguments
    ASSERT(!extraArgs);
    // Now that locals have been initialized, it is safe to attach
    // the VarEnv inherited from our caller to the current frame
    ar->m_varEnv->attach(ar);
  } else if (extraArgs) {
    // Create a new VarEnv and stash the extra args in there
    int numExtras = ar->numArgs() - ar->m_func->numParams();
    ASSERT(numExtras > 0);
    ar->m_varEnv = VarEnv::createLazyAttach(ar);
    ar->m_varEnv->setExtraArgs(extraArgs, numExtras);
  }

  ar->m_savedRip = (uintptr_t)m_transl->getCallToExit();
  m_firstAR = ar;
  m_halted = false;

  if (enter) {
    jmp_buf buf;
    m_jmpBufs.push_back(&buf);
    Util::compiler_membar();
    switch (setjmp(buf)) {
    case SETJMP:
      Util::compiler_membar();
      enterVMWork(ar, true);
      break;
    case LONGJUMP_PROPAGATE:
      Util::compiler_membar();
      m_jmpBufs.pop_back();
      ASSERT(m_faults.size() > 0);
      {
        Fault fault = m_faults.back();
        m_faults.pop_back();
        switch (fault.m_faultType) {
          case Fault::KindOfUserException: {
            Object obj = fault.m_userException;
            fault.m_userException->decRefCount();
            throw obj;
          }
          case Fault::KindOfCPPException: {
            // throwException() will take care of deleting heap-allocated
            // exception object for us
            fault.m_cppException->throwException();
          }
          default: {
            not_implemented();
          }
        }
      }
      NOT_REACHED();
      break;
    case LONGJUMP_RESUMEVM:
      Util::compiler_membar();
      enterVMWork(ar, false);
      break;
    case LONGJUMP_DEBUGGER:
      // Triggered by switchMode() to switch VM mode
      // do nothing but reenter the VM with same VM stack
      ar = m_fp;
      enterVMWork(ar, false);
      break;
    default:
      NOT_REACHED();
    }
    m_jmpBufs.pop_back();
  }

  m_nestedVMMap.erase(ar);
  m_halted = false;

  memcpy(retval, m_stack.topTV(), sizeof(TypedValue));
  m_stack.discard();
}

void VMExecutionContext::reenterVM(TypedValue* retval,
                                   ActRec* ar,
                                   TypedValue* extraArgs,
                                   TypedValue* savedSP) {
  ar->m_soff = 0;
  ar->m_savedRbp = 0;
  VMState savedVM = { m_pc, m_fp, m_firstAR, savedSP };
  TRACE(3, "savedVM: %p %p %p %p\n", m_pc, m_fp, m_firstAR, savedSP);
  pushVMState(savedVM);
  ASSERT(m_nestedVMs.size() >= 1);
  enterVM(retval, ar, extraArgs);
  m_pc = savedVM.pc;
  m_fp = savedVM.fp;
  m_firstAR = savedVM.firstAR;
  ASSERT(m_stack.top() == savedVM.sp);
  popVMState();
  TRACE(1, "Reentry: exit fp %p pc %p\n", m_fp, m_pc);
}

int VMExecutionContext::switchMode(bool unwindBuiltin) {
  if (unwindBuiltin) {
    // from Jit calling a builtin, should unwind a frame, and push a
    // return value on stack
    tx64->sync(); // just to set tl_regState
    unwindBuiltinFrame();
    m_stack.pushNull();
  }
  return LONGJUMP_DEBUGGER;
}

void VMExecutionContext::invokeFunc(TypedValue* retval,
                                    const Func* f,
                                    CArrRef params,
                                    ObjectData* this_ /* = NULL */,
                                    Class* cls /* = NULL */,
                                    VarEnv* varEnv /* = NULL */,
                                    StringData* invName /* = NULL */,
                                    Unit* toMerge /* = NULL */) {
  ASSERT(retval);
  ASSERT(f);
  // If this is a regular function, this_ and cls must be NULL
  ASSERT(f->preClass() || f->isPseudoMain() || (!this_ && !cls));
  // If this is a method, either this_ or cls must be non-NULL
  ASSERT(!f->preClass() || (this_ || cls));
  // If this is a static method, this_ must be NULL
  ASSERT(!(f->attrs() & HPHP::VM::AttrStatic) || (!this_));
  // invName should only be non-NULL if we are calling __call or
  // __callStatic
  ASSERT(!invName || f->name()->isame(s___call.get()) ||
         f->name()->isame(s___callStatic.get()));
  // If a variable environment is being inherited then params must be empty
  ASSERT(!varEnv || params.empty());

  VMRegAnchor _;

  // Check if we need to run an intercept handler
  if (UNLIKELY(f->maybeIntercepted())) {
    Variant *h = get_intercept_handler(f->fullNameRef(),
                                       &f->maybeIntercepted());
    if (h) {
      if (!run_intercept_handler_for_invokefunc(retval, f, params, this_,
                                                invName, h)) {
        return;
      }
      // Discard the handler's return value
      tvRefcountedDecRef(retval);
    }
  } 

  bool isMagicCall = (invName != NULL);

  if (this_ != NULL) {
    this_->incRefCount();
  }
  Cell* savedSP = m_stack.top();

  checkStack(m_stack, f);

  ActRec* ar = m_stack.allocA();
  ar->m_savedRbp = 0;
  ar->m_func = f;
  if (this_) {
    ar->setThis(this_);
  } else if (cls) {
    ar->setClass(cls);
  } else {
    ar->setThis(NULL);
  }
  if (isMagicCall) {
    ar->initNumArgs(2);
  } else {
    ar->initNumArgs(params.size());
  }
  ar->setVarEnv(varEnv);

#ifdef HPHP_TRACE
  if (m_fp == NULL) {
    TRACE(1, "Reentry: enter %s(%p) from top-level\n",
          f->name()->data(), ar);
  } else {
    TRACE(1, "Reentry: enter %s(pc %p ar %p) from %s(%p)\n",
          f->name()->data(), m_pc, ar,
          m_fp->m_func ? m_fp->m_func->name()->data() : "unknownBuiltin", m_fp);
  }
#endif

  HphpArray *arr = dynamic_cast<HphpArray*>(params.get());
  TypedValue* extraArgs = NULL;
  if (isMagicCall) {
    // Put the method name into the location of the first parameter. We
    // are transferring ownership, so no need to incRef/decRef here.
    m_stack.pushStringNoRc(invName);
    // Put array of arguments into the location of the second parameter
    m_stack.pushArray(arr);
  } else {
    Array hphpArrCopy(StaticEmptyHphpArray::Get());
    if (UNLIKELY(!arr) && !params.empty()) {
      // empty() check needed because we sometimes represent empty arrays
      // as smart pointers with m_px == NULL, which freaks out
      // ArrayData::merge.
      hphpArrCopy.merge(params);
      arr = dynamic_cast<HphpArray*>(hphpArrCopy.get());
      ASSERT(arr && IsHphpArray(arr));
    }
    if (arr) {
      int numParams = f->numParams();
      int numExtraArgs = arr->size() - numParams;
      if (numExtraArgs > 0) {
        extraArgs = (TypedValue*)malloc(sizeof(TypedValue) * numExtraArgs);
      }
      int paramId = 0;
      for (ssize_t i = arr->iter_begin();
           i != ArrayData::invalid_index;
           i = arr->iter_advance(i), ++paramId) {
        TypedValue *from = arr->nvGetValueRef(i);
        TypedValue *to;
        if (LIKELY(paramId < numParams)) {
          to = m_stack.allocTV();
        } else {
          ASSERT(extraArgs && numExtraArgs > 0);
          // VarEnv expects the extra args to be in "reverse" order
          // (i.e. the last extra arg has the lowest address)
          to = extraArgs + (numExtraArgs - 1) - (paramId - numParams);
        }
        if (LIKELY(!f->byRef(paramId))) {
          tvDup(from, to);
          if (to->m_type == KindOfVariant) {
            tvUnbox(to);
          }
        } else {
          if (from->m_type != KindOfVariant) {
            tvBox(from);
          }
          tvDup(from, to);
        }
      }
    }
  }

  if (toMerge != NULL) {
    mergeUnit(toMerge);
  }

  if (m_fp) {
    reenterVM(retval, ar, extraArgs, savedSP);
  } else {
    ASSERT(m_nestedVMs.size() == 0);
    enterVM(retval, ar, extraArgs);
  }
}

void VMExecutionContext::invokeUnit(TypedValue* retval, Unit* unit) {
  Func* func = unit->getMain();
  VarEnv* varEnv;
  if (g_vmContext->m_varEnvs.empty()) {
    // The global variable environment hasn't been created yet, so
    // create it
    varEnv = VarEnv::createGlobal();
  } else {
    // Get the global variable environment
    varEnv = g_vmContext->m_varEnvs.front();
  }
  invokeFunc(retval, func, Array::Create(), NULL, NULL, varEnv, NULL, unit);
}


Variant i_callUserFunc(void *extra, CArrRef params) {
  Variant v;
  g_vmContext->invokeFunc((TypedValue*)&v, (Func*)extra, params);
  return v;
}

// XXX hackity hack hack
// This is solely here to support generator methods. The C++ implementation of
// the Continuation class is the only thing that uses this function.
Variant i_callUserFunc1ArgMCP(MethodCallPackage& mcp, int num, CVarRef arg0) {
  assert(false);
  return Variant();
}

CallInfoWithConstructor ci_callUserFunc(
  (void *)&i_callUserFunc, (void *)&i_callUserFunc1ArgMCP,
  0, 0, 0);

bool VMExecutionContext::getCallInfo(const CallInfo *&outCi,
                                     void *&outExtra, const char *s) {
  StringData funcName(s);
  Func* func = Unit::lookupFunc(&funcName);
  if (!func) {
    return false;
  }
  outExtra = (void*)func;
  outCi = &ci_callUserFunc;
  return true;
}

bool VMExecutionContext::getCallInfoStatic(const CallInfo*& outCi,
                                           void*& outExtra,
                                           const StringData* clsName,
                                           const StringData* funcName) {
  Class* cls = Unit::lookupClass(clsName);
  if (cls == NULL) {
    return false;
  }
  const Func* method = cls->lookupMethod(funcName);
  if (method == NULL) {
    return false;
  }
  outExtra = (void *)method;
  outCi = &ci_callUserFunc;
  return true;
}

void VMExecutionContext::unwindBuiltinFrame() {
  // Unwind the frame for a builtin. Currently only used for
  // hphpd_break and fb_enable_code_coverage
  ASSERT(m_fp->m_func->isBuiltin());
  ASSERT(m_fp->m_func->name()->isame(s_hphpd_break.get()) ||
         m_fp->m_func->name()->isame(s_fb_enable_code_coverage.get()));
  // Free any values that may be on the eval stack
  TypedValue *evalTop = (TypedValue*)m_fp;
  while (m_stack.topTV() < evalTop) {
    m_stack.popTV();
  }
  // Free the locals and VarEnv if there is one
  frame_free_locals_inl(m_fp, m_fp->m_func->numLocals());
  // Tear down the frame
  Offset pc = -1;
  ActRec* sfp = getPrevVMState(m_fp, &pc);
  ASSERT(pc != -1);
  m_fp = sfp;
  m_pc = m_fp->m_func->unit()->at(pc);
  m_stack.discardAR();
}

void VMExecutionContext::hhvmThrow(int longJumpType) {
  jmp_buf *buf = m_jmpBufs.back();
  longjmp(*buf, longJumpType);
}

int VMExecutionContext::hhvmPrepareThrow() {
  Fault fault = m_faults.back();
  tx64->sync();
  TRACE(2, "hhvmThrow: %p(\"%s\") {\n", m_fp, m_fp->m_func->name()->data());
  UnwindStatus unwindType;
  unwindType = m_stack.unwindFrame(m_fp, pcOff(),
                                   m_pc, fault);
  return handleUnwind(unwindType);
}

/*
 * Given a pointer to a VM frame, returns the previous VM frame in the call
 * stack. This function will also pass back by reference the previous PC (if
 * prevPc is non-null) and the previous SP (if prevSp is non-null).
 *
 * If there is no previous VM frame, this function returns NULL and does not
 * set prevPc and prevSp.
 */
ActRec* VMExecutionContext::getPrevVMState(const ActRec* fp,
                                           Offset*       prevPc /* = NULL */,
                                           TypedValue**  prevSp /* = NULL */) {
  if (fp == NULL) {
    return NULL;
  }
  ActRec* prevFp = arGetSfp(fp);
  if (prevFp != fp) {
    if (prevSp) *prevSp = (TypedValue*)&fp[1];
    if (prevPc) *prevPc = prevFp->m_func->base() + fp->m_soff;
    return prevFp;
  }
  ASSERT(m_nestedVMMap.find(fp) != m_nestedVMMap.end());
  int k = m_nestedVMMap[fp];
  if (k < 0) {
    return NULL;
  }
  ASSERT(k < (int)m_nestedVMs.size());
  prevFp = m_nestedVMs[k].fp;
  ASSERT(prevFp);
  ASSERT(prevFp->m_func->unit());
  if (prevSp) *prevSp = m_nestedVMs[k].sp;
  if (prevPc) {
    *prevPc = prevFp->m_func->unit()->offsetOf(m_nestedVMs[k].pc);
  }
  return prevFp;
}

Array VMExecutionContext::debugBacktrace(bool skip /* = false */,
                                         bool withSelf /* = false */,
                                         bool withThis /* = false */,
                                         VMParserFrame*
                                         parserFrame /* = NULL */) {
  static StringData* s_file = StringData::GetStaticString("file");
  static StringData* s_line = StringData::GetStaticString("line");
  static StringData* s_function = StringData::GetStaticString("function");
  static StringData* s_args = StringData::GetStaticString("args");
  static StringData* s_class = StringData::GetStaticString("class");
  static StringData* s_object = StringData::GetStaticString("object");
  static StringData* s_type = StringData::GetStaticString("type");

  Array bt = Array::Create();

  // If there is a parser frame, put it at the beginning of
  // the backtrace
  if (parserFrame) {
    Array frame = Array::Create();
    frame.set(String(s_file), parserFrame->filename, true);
    frame.set(String(s_line), parserFrame->lineNumber, true);
    bt.append(frame);
  }
  if (!m_fp) {
    // If there are no VM frames, we're done
    return bt;
  }
  Transl::VMRegAnchor _;
  // Get the fp and pc of the top frame (possibly skipping one frame)
  ActRec* fp;
  Offset pc;
  if (skip) {
    fp = getPrevVMState(m_fp, &pc);
    if (!fp) {
      // We skipped over the only VM frame, we're done
      return bt;
    }
  } else {
    fp = m_fp;
    Unit *unit = m_fp->m_func->unit();
    ASSERT(unit);
    pc = unit->offsetOf(m_pc);
  }
  // Handle the top frame
  if (withSelf) {
    // Builtins don't have a file and line number
    if (!fp->m_func->isBuiltin()) {
      Unit *unit = fp->m_func->unit();
      ASSERT(unit);
      const char* filename = unit->filepath()->data();
      ASSERT(filename);
      Offset off = pc;
      Array frame = Array::Create();
      frame.set(String(s_file), filename, true);
      frame.set(String(s_line), unit->getLineNumber(off), true);
      if (parserFrame) {
        frame.set(String(s_function), "include", true);
        frame.set(String(s_args), Array::Create(parserFrame->filename), true);
      }
      bt.append(frame);
    }
  }
  // Handle the subsequent VM frames
  for (ActRec* prevFp = getPrevVMState(fp, &pc); fp != NULL;
       fp = prevFp, prevFp = getPrevVMState(fp, &pc)) {
    Array frame = Array::Create();
    // do not capture frame for HPHP only functions
    if (fp->m_func->isNoInjection()) {
      continue;
    }
    // Builtins don't have a file and line number
    if (prevFp && !prevFp->m_func->isBuiltin()) {
      Unit* unit = prevFp->m_func->unit();
      ASSERT(unit);
      const char *filename = unit->filepath()->data();
      ASSERT(filename);
      frame.set(String(s_file), filename, true);
      frame.set(String(s_line),
                prevFp->m_func->unit()->getLineNumber(pc), true);
    }
    // check for include
    StringData *funcname = const_cast<StringData*>(fp->m_func->name());
    if (fp->m_func->isClosureBody()) {
      static StringData* s_closure_label =
          StringData::GetStaticString("{closure}");
      funcname = s_closure_label;
    }
    // check for pseudomain
    if (funcname->empty()) {
      continue;
    }
    frame.set(String(s_function), String(funcname), true);
    // Closures have an m_this but they aren't in object context
    Class* ctx = arGetContextClass(fp);
    if (ctx != NULL && !fp->m_func->isClosureBody()) {
      frame.set(String(s_class), ctx->name()->data(), true);
      if (fp->hasThis()) {
        if (withThis) {
          frame.set(String(s_object), Object(fp->getThis()), true);
        }
        frame.set(String(s_type), "->", true);
      } else {
        frame.set(String(s_type), "::", true);
      }
    }
    Array args = Array::Create();
    int nparams = fp->m_func->numParams();
    int nargs = fp->numArgs();
    /* builtin extra args are not stored in varenv */
    if (nargs <= nparams) {
      for (int i = 0; i < nargs; i++) {
        TypedValue *arg = frame_local(fp, i);
        args.append(tvAsVariant(arg));
      }
    } else {
      int i;
      for (i = 0; i < nparams; i++) {
        TypedValue *arg = frame_local(fp, i);
        args.append(tvAsVariant(arg));
      }
      for (; i < nargs; i++) {
        ASSERT(fp->hasVarEnv());
        TypedValue *arg = fp->getVarEnv()->getExtraArg(i - nparams);
        args.append(tvAsVariant(arg));
      }
    }
    frame.set(String(s_args), args, true);
    bt.append(frame);
  }
  return bt;
}

/*
   Attempt to instantiate all the hoistable classes
   and functions.
   returns true on success
*/
bool VMExecutionContext::mergeUnit(Unit* unit) {
  unit->mergePreConsts();
  unit->mergeFuncs();
  return unit->mergeClasses();
}

MethodInfoVM::~MethodInfoVM() {
  for (std::vector<const ClassInfo::ParameterInfo*>::iterator it =
       parameters.begin(); it != parameters.end(); ++it) {
    if ((*it)->value != NULL) {
      free((void*)(*it)->value);
    }
  }
}

ClassInfoVM::~ClassInfoVM() {
  destroyMembers(m_methodsVec);
  destroyMapValues(m_properties);
  destroyMapValues(m_constants);
}

Array VMExecutionContext::getUserFunctionsInfo() {
  // Return an array of all user-defined function names.  This method is used to
  // support get_defined_functions().
  return Unit::getUserFunctions();
}

Array VMExecutionContext::getConstantsInfo() {
  // Return an array of all defined constant:value pairs.  This method is used
  // to support get_defined_constants().
  return Array(m_constants.copy());
}

const ClassInfo::MethodInfo* VMExecutionContext::findFunctionInfo(
  CStrRef name) {
  StringIMap<AtomicSmartPtr<MethodInfoVM> >::iterator it =
    m_functionInfos.find(name);
  if (it == m_functionInfos.end()) {
    Func* func = Unit::lookupFunc(name.get());
    if (func == NULL || func->builtinFuncPtr()) {
      // Fall back to the logic in ClassInfo::FindFunction() logic to deal
      // with builtin functions
      return NULL;
    }
    AtomicSmartPtr<MethodInfoVM> &m = m_functionInfos[name];
    m = new MethodInfoVM();
    func->getFuncInfo(m.get());
    return m.get();
  } else {
    return it->second.get();
  }
}

const ClassInfo* VMExecutionContext::findClassInfo(CStrRef name) {
  if (name->empty()) return NULL;
  StringIMap<AtomicSmartPtr<ClassInfoVM> >::iterator it =
    m_classInfos.find(name);
  if (it == m_classInfos.end()) {
    Class* cls = Unit::lookupClass(name.get());
    if (cls == NULL) return NULL;
    if (cls->clsInfo()) return cls->clsInfo();
    if (cls->attrs() & (AttrInterface | AttrTrait)) {
      // If the specified name matches with something that is not formally
      // a class, return NULL
      return NULL;
    }
    AtomicSmartPtr<ClassInfoVM> &c = m_classInfos[name];
    c = new ClassInfoVM();
    cls->getClassInfo(c.get());
    return c.get();
  } else {
    return it->second.get();
  }
}

const ClassInfo* VMExecutionContext::findInterfaceInfo(CStrRef name) {
  StringIMap<AtomicSmartPtr<ClassInfoVM> >::iterator it =
    m_interfaceInfos.find(name);
  if (it == m_interfaceInfos.end()) {
    Class* cls = Unit::lookupClass(name.get());
    if (cls == NULL)  return NULL;
    if (cls->clsInfo()) return cls->clsInfo();
    if (!(cls->attrs() & AttrInterface)) {
      // If the specified name matches with something that is not formally
      // an interface, return NULL
      return NULL;
    }
    AtomicSmartPtr<ClassInfoVM> &c = m_interfaceInfos[name];
    c = new ClassInfoVM();
    cls->getClassInfo(c.get());
    return c.get();
  } else {
    return it->second.get();
  }
}

const ClassInfo* VMExecutionContext::findTraitInfo(CStrRef name) {
  StringIMap<AtomicSmartPtr<ClassInfoVM> >::iterator it =
    m_traitInfos.find(name);
  if (it != m_traitInfos.end()) {
    return it->second.get();
  }
  Class* cls = Unit::lookupClass(name.get());
  if (cls == NULL) return NULL;
  if (cls->clsInfo()) return cls->clsInfo();
  if (!(cls->attrs() & AttrTrait)) {
    return NULL;
  }
  AtomicSmartPtr<ClassInfoVM> &classInfo = m_traitInfos[name];
  classInfo = new ClassInfoVM();
  cls->getClassInfo(classInfo.get());
  return classInfo.get();
}

const ClassInfo::ConstantInfo* VMExecutionContext::findConstantInfo(
    CStrRef name) {
  TypedValue* tv = m_constants.nvGet(name.get());
  if (tv == NULL) {
    return NULL;
  }
  ConstInfoMap::const_iterator it = m_constInfo.find(name.get());
  if (it != m_constInfo.end()) {
    return it->second;
  }
  StringData* key = StringData::GetStaticString(name.get());
  ClassInfo::ConstantInfo* ci = new ClassInfo::ConstantInfo();
  ci->name = *(const String*)&key;
  ci->valueLen = 0;
  ci->valueText = "";
  ci->setValue(tvAsCVarRef(tv));
  m_constInfo[key] = ci;
  return ci;
}

struct ResolveIncludeContext {
  String path; // translated path of the file
  struct stat* s; // stat for the file
};

static bool findFileWrapper(CStrRef file, void* ctx) {
  ResolveIncludeContext* context = (ResolveIncludeContext*)ctx;
  ASSERT(context->path.isNull());
  // TranslatePath() will canonicalize the path and also check
  // whether the file is in an allowed directory.
  String translatedPath = File::TranslatePath(file, false, true);
  if (file[0] != '/') {
    if (HPHP::Eval::FileRepository::findFile(translatedPath.get(),
                                             context->s)) {
      context->path = translatedPath;
      return true;
    }
    return false;
  }
  if (RuntimeOption::SandboxMode || !RuntimeOption::AlwaysUseRelativePath) {
    if (HPHP::Eval::FileRepository::findFile(translatedPath.get(),
                                             context->s)) {
      context->path = translatedPath;
      return true;
    }
  }
  string server_root(SourceRootInfo::GetCurrentSourceRoot());
  if (server_root.empty()) {
    server_root = string(g_vmContext->getCwd()->data());
    if (server_root.empty() || server_root[server_root.size() - 1] != '/') {
      server_root += "/";
    }
  }
  String rel_path(Util::relativePath(server_root, translatedPath.data()));
  if (HPHP::Eval::FileRepository::findFile(rel_path.get(),
                                           context->s)) {
    context->path = rel_path;
    return true;
  }
  return false;
}

HPHP::Eval::PhpFile* VMExecutionContext::lookupPhpFile(StringData* path,
                                                       const char* currentDir,
                                                       bool* initial_opt) {
  bool init;
  bool &initial = initial_opt ? *initial_opt : init;
  initial = true;

  struct stat s;
  String spath;
  // Resolve the include path
  {
    ResolveIncludeContext ctx;
    ctx.s = &s;
    resolve_include(path, currentDir, findFileWrapper,
                    (void*)&ctx);
    // If resolve_include() could not find the file, return NULL
    if (ctx.path.isNull()) {
      return NULL;
    }
    spath = ctx.path;
  }
  // Check if this file has already been included.
  EvaledFilesMap::const_iterator it = m_evaledFiles.find(spath.get());
  HPHP::Eval::PhpFile* efile = NULL;
  if (it != m_evaledFiles.end()) {
    // We found it! Return the unit.
    efile = it->second;
    initial = false;
    if (!initial_opt) efile->incRef();
    return efile;
  }
  // We didn't find it, so try the realpath.
  bool alreadyResolved =
    RuntimeOption::RepoAuthoritative ||
    (!RuntimeOption::CheckSymLink && (spath[0] == '/'));
  bool hasRealpath = false;
  String rpath;
  if (!alreadyResolved) {
    std::string rp = StatCache::realpath(spath.data());
    if (rp.size() != 0) {
      rpath = NEW(StringData)(rp.data(), rp.size(), CopyString);
      if (!rpath.same(spath)) {
        hasRealpath = true;
        it = m_evaledFiles.find(rpath.get());
        if (it != m_evaledFiles.end()) {
          // We found it! Update the mapping for spath and
          // return the unit.
          efile = it->second;
          m_evaledFiles[spath.get()] = efile;
          spath.get()->incRefCount();
          efile->incRef();
          initial = false;
          if (!initial_opt) efile->incRef();
          return efile;
        }
      }
    }
  }
  // This file hasn't been included yet, so we need to parse the file
  efile = HPHP::Eval::FileRepository::checkoutFile(
    hasRealpath ? rpath.get() : spath.get(), s);
  ASSERT(!efile || efile->getRef() > 0);
  if (efile && initial_opt) {
    // if initial_opt is not set, this shouldnt be recorded as a
    // per request fetch of the file.
    if (Transl::TargetCache::testAndSetBit(efile->getId())) {
      initial = false;
    }
    // if parsing was successful, update the mappings for spath and
    // rpath (if it exists).
    m_evaledFiles[spath.get()] = efile;
    spath.get()->incRefCount();
    // Don't incRef efile; checkoutFile() already counted it.
    if (hasRealpath) {
      m_evaledFiles[rpath.get()] = efile;
      rpath.get()->incRefCount();
      efile->incRef();
    }
    DEBUGGER_ATTACHED_ONLY(phpFileLoadHook(efile));
  }
  return efile;
}

Unit* VMExecutionContext::evalInclude(StringData* path,
                                      const StringData* curUnitFilePath,
                                      bool* initial) {
  namespace fs = boost::filesystem;
  fs::path currentUnit(curUnitFilePath->data());
  fs::path currentDir(currentUnit.branch_path());
  HPHP::Eval::PhpFile* efile =
    lookupPhpFile(path, currentDir.string().c_str(), initial);
  if (efile) {
    return efile->unit();
  }
  return NULL;
}

HPHP::VM::Unit* VMExecutionContext::evalIncludeRoot(
  StringData* path, InclOpFlags flags, bool* initial) {
  HPHP::Eval::PhpFile* efile = lookupIncludeRoot(path, flags, initial);
  return efile ? efile->unit() : 0;
}

HPHP::Eval::PhpFile* VMExecutionContext::lookupIncludeRoot(StringData* path,
                                                           InclOpFlags flags,
                                                           bool* initial) {
  String absPath;
  if ((flags & InclOpRelative)) {
    namespace fs = boost::filesystem;
    fs::path currentUnit(m_fp->m_func->unit()->filepath()->data());
    fs::path currentDir(currentUnit.branch_path());
    absPath = currentDir.string() + '/';
    TRACE(2, "lookupIncludeRoot(%s): relative -> %s\n",
          path->data(),
          absPath->data());
  } else {
    ASSERT(flags & InclOpDocRoot);
    absPath = SourceRootInfo::GetCurrentPhpRoot();
    TRACE(2, "lookupIncludeRoot(%s): docRoot -> %s\n",
          path->data(),
          absPath->data());
  }

  absPath += StrNR(path);

  EvaledFilesMap::const_iterator it = m_evaledFiles.find(absPath.get());
  if (it != m_evaledFiles.end()) {
    if (initial) *initial = false;
    if (!initial) it->second->incRef();
    return it->second;
  }

  return lookupPhpFile(absPath.get(), "", initial);
}

/*
  Instantiate hoistable classes and functions.
  If there is any more work left to do, setup a
  new frame ready to execute the pseudomain.

  return true iff the pseudomain needs to be executed.
*/
bool VMExecutionContext::evalUnit(Unit* unit, bool local,
                                  PC& pc, int funcType) {
  if (mergeUnit(unit)) {
    if (unit->getMainAttrs() & AttrMergeOnly) {
      Stats::inc(Stats::ExecPS_Skipped);
      m_stack.pushInt(1);
      return false;
    }
    Stats::inc(Stats::ExecPS_Always);
  } else {
    Stats::inc(unit->getMainAttrs() & AttrMergeOnly ?
               Stats::ExecPS_MergeFailed : Stats::ExecPS_Always);
  }

  Func* func = unit->getMain();
  ActRec* ar = m_stack.allocA();
  ASSERT((uintptr_t)&ar->m_func < (uintptr_t)&ar->m_r);
  ASSERT(!func->isBuiltin());
  ar->m_func = func;
  ar->initNumArgs(0);
  ASSERT(m_fp);
  ASSERT(!m_fp->hasInvName());
  if (m_fp->hasThis()) {
    ObjectData *this_ = m_fp->getThis();
    this_->incRefCount();
    ar->setThis(this_);
  } else if (m_fp->hasClass()) {
    ar->setClass(m_fp->getClass());
  } else {
    ar->setThis(NULL);
  }
  arSetSfp(ar, m_fp);
  ar->m_soff = uintptr_t(m_fp->m_func->unit()->offsetOf(pc) -
                         m_fp->m_func->base());
  ar->m_savedRip = (uintptr_t)m_transl->getRetFromInterpretedFrame();
  pushLocalsAndIterators(func);
  if (local) {
    ar->m_varEnv = 0;
  } else {
    if (m_fp->m_varEnv == NULL) {
      m_fp->m_varEnv = VarEnv::createLazyAttach(m_fp);
    }
    ar->m_varEnv = m_fp->m_varEnv;
    ar->m_varEnv->attach(ar);
  }
  m_fp = ar;
  pc = func->getEntry();
  SYNC();
  EventHook::FunctionEnter(m_fp, funcType);
  return true;
}

CVarRef VMExecutionContext::getEvaledArg(const StringData* val) {
  CStrRef key = *(String*)&val;

  if (m_evaledArgs.get()) {
    CVarRef arg = m_evaledArgs.get()->get(key);
    if (&arg != &null_variant) return arg;
  }
  String code = HPHP::concat3("<?php return ", key, ";");
  VM::Unit* unit = compileEvalString(code.get());
  ASSERT(unit != NULL);
  Variant v;
  g_vmContext->invokeFunc((TypedValue*)&v, unit->getMain(),
                          Array::Create());
  Variant &lv = m_evaledArgs.lvalAt(key, AccessFlags::Key);
  lv = v;
  return lv;
}

/*
 * Helper for function entry, including pseudo-main entry.
 */
inline void
VMExecutionContext::pushLocalsAndIterators(const Func* func,
                                           int nparams /*= 0*/) {
  // Push locals.
  for (int i = nparams; i < func->numLocals(); i++) {
    m_stack.pushUninit();
  }
  // Push iterators.
  for (int i = 0; i < func->numIterators(); i++) {
    m_stack.allocI();
  }
}

void VMExecutionContext::destructObjects() {
  if (UNLIKELY(RuntimeOption::EnableObjDestructCall)) {
    while (!m_liveBCObjs.empty()) {
      ObjectData* o = *m_liveBCObjs.begin();
      Instance* instance = static_cast<Instance*>(o);
      ASSERT(o->isInstance());
      instance->destruct(); // Let the instance remove the node.
    }
    m_liveBCObjs.clear();
  }
}

// Evaled units have a footprint in the TC and translation metadata. The
// applications we care about tend to have few, short, stereotyped evals,
// where the same code keeps getting eval'ed over and over again; so we
// keep around units for each eval'ed string, so that the TC space isn't
// wasted on each eval.
typedef RankedCHM<StringData*, HPHP::VM::Unit*,
        StringDataHashCompare,
        RankEvaledUnits> EvaledUnitsMap;
static EvaledUnitsMap s_evaledUnits;
Unit* VMExecutionContext::compileEvalString(StringData* code) {
  EvaledUnitsMap::accessor acc;
  // Promote this to a static string; otherwise it may get swept
  // across requests.
  code = StringData::GetStaticString(code);
  if (s_evaledUnits.insert(acc, code)) {
    acc->second = compile_string(code->data(), code->size());
  }
  return acc->second;
}

CStrRef VMExecutionContext::createFunction(CStrRef args, CStrRef code) {
  // It doesn't matter if there's a user function named __lambda_func; we only
  // use this name during parsing, and then change it to an impossible name
  // with a NUL byte before we merge it into the request's func map.  This also
  // has the bonus feature that the value of __FUNCTION__ inside the created
  // function will match Zend. (Note: Zend will actually fatal if there's a
  // user function named __lambda_func when you call create_function. Huzzah!)
  static StringData* oldName = StringData::GetStaticString("__lambda_func");
  std::ostringstream codeStr;
  codeStr << "<?php function " << oldName->data()
          << "(" << args.data() << ") {"
          << code.data() << "}\n";
  StringData* evalCode = StringData::GetStaticString(codeStr.str());
  Unit* unit = VM::compile_string(evalCode->data(), evalCode->size());
  // Move the function to a different name.
  std::ostringstream newNameStr;
  newNameStr << '\0' << "lambda_" << ++m_lambdaCounter;
  StringData* newName = StringData::GetStaticString(newNameStr.str());
  unit->renameFunc(oldName, newName);
  m_createdFuncs.push_back(unit);
  unit->mergePreConsts();
  unit->mergeFuncs();

  // Technically we shouldn't have to eval the unit right now (it'll execute
  // the pseudo-main, which should be empty) and could get away with just
  // mergeFuncs. However, Zend does it this way, as proven by the fact that you
  // can inject code into the evaled unit's pseudo-main:
  //
  //   create_function('', '} echo "hi"; if (0) {');
  //
  // We have to eval now to emulate this behavior.
  TypedValue retval;
  invokeFunc(&retval, unit->getMain(), Array::Create());

  return unit->getLambda()->nameRef();
}

void VMExecutionContext::evalPHPDebugger(TypedValue* retval, StringData *code,
                                         int frame) {
  ASSERT(retval);
  // The code has "<?php" prepended already
  Unit* unit = compileEvalString(code);
  if (unit == NULL) {
    raise_error("Syntax error");
    tvWriteNull(retval);
    return;
  }

  VarEnv *varEnv = NULL;
  ActRec *fp = m_fp;
  ActRec *cfpSave = NULL;
  if (fp) {
    ASSERT(!g_vmContext->m_varEnvs.empty());
    std::list<HPHP::VM::VarEnv*>::iterator it = g_vmContext->m_varEnvs.end();
    for (; frame > 0; --frame) {
      if (fp->m_varEnv != NULL) {
        if (it == g_vmContext->m_varEnvs.end() || *it != fp->m_varEnv) {
          --it;
        }
        ASSERT(*it == fp->m_varEnv);
      }
      ActRec *prevFp = getPrevVMState(fp);
      if (!prevFp) {
        // To be safe in case we failed to get prevFp
        break;
      }
      fp = prevFp;
    }
    if (fp->m_varEnv == NULL) {
      const bool skipInsert = true;
      fp->m_varEnv = VarEnv::createLazyAttach(fp, skipInsert);
      g_vmContext->m_varEnvs.insert(it, fp->m_varEnv);
    }
    varEnv = fp->m_varEnv;
    cfpSave = varEnv->getCfp();
  }
  ObjectData *this_ = NULL;
  Class *cls = NULL;
  if (fp) {
    if (fp->hasThis()) {
      this_ = fp->getThis();
    } else if (fp->hasClass()) {
      cls = fp->getClass();
    }
  }

  const static StaticString s_cppException("Hit an exception");
  const static StaticString s_phpException("Hit a php exception");
  const static StaticString s_exit("Hit exit");
  const static StaticString s_fatal("Hit fatal");
  try {
    invokeFunc(retval, unit->getMain(), Array::Create(), this_, cls,
               varEnv, NULL, unit);
  } catch (FatalErrorException &e) {
    g_vmContext->write(s_fatal);
    g_vmContext->write(" : ");
    g_vmContext->write(e.getMessage().c_str());
    g_vmContext->write("\n");
    g_vmContext->write(ExtendedLogger::StringOfStackTrace(*e.getBackTrace()));
  } catch (ExitException &e) {
    g_vmContext->write(s_exit.data());
    g_vmContext->write(" : ");
    std::ostringstream os;
    os << ExitException::ExitCode;
    g_vmContext->write(os.str());
  } catch (Eval::DebuggerException &e) {
    if (varEnv) {
      varEnv->setCfp(cfpSave);
    }
    throw;
  } catch (Exception &e) {
    g_vmContext->write(s_cppException.data());
    g_vmContext->write(" : ");
    g_vmContext->write(e.getMessage().c_str());
    ExtendedException* ee = dynamic_cast<ExtendedException*>(&e);
    if (ee) {
      g_vmContext->write("\n");
      g_vmContext->write(
        ExtendedLogger::StringOfStackTrace(*ee->getBackTrace()));
    }
  } catch (Object &e) {
    g_vmContext->write(s_phpException.data());
    g_vmContext->write(" : ");
    g_vmContext->write(e->t___tostring().data());
  } catch (...) {
    g_vmContext->write(s_cppException.data());
  }

  if (varEnv) {
    // Set the Cfp back
    varEnv->setCfp(cfpSave);
  }
}

void VMExecutionContext::enterDebuggerDummyEnv() {
  static Unit* s_debuggerDummy = NULL;
  if (!s_debuggerDummy) {
    s_debuggerDummy = compile_string("<?php?>", 7);
  }
  VarEnv* varEnv = NULL;
  if (g_vmContext->m_varEnvs.empty()) {
    varEnv = VarEnv::createGlobal();
  } else {
    varEnv = g_vmContext->m_varEnvs.back();
  }
  if (!m_fp) {
    ASSERT(m_stack.count() == 0);
    ActRec* ar = m_stack.allocA();
    ar->m_func = s_debuggerDummy->getMain();
    ar->setThis(NULL);
    ar->m_soff = 0;
    ar->m_savedRbp = 0;
    ar->m_savedRip = (uintptr_t)m_transl->getCallToExit();
    m_fp = ar;
    m_pc = s_debuggerDummy->entry();
    m_firstAR = ar;
    m_nestedVMMap[ar] = m_nestedVMs.size() - 1;
    m_halted = false;
  }
  m_fp->setVarEnv(varEnv);
  varEnv->attach(m_fp);
}

void VMExecutionContext::exitDebuggerDummyEnv() {
  ASSERT(!g_vmContext->m_varEnvs.empty());
  ASSERT(g_vmContext->m_varEnvs.front() == g_vmContext->m_varEnvs.back());
  VarEnv* varEnv = g_vmContext->m_varEnvs.front();
  varEnv->detach(m_fp);
}

#define LOOKUP_NAME(name, key) (name) = prepareKey(key).detach()
#define LOOKUP_VAR(name, key, val) do {                                       \
  LOOKUP_NAME(name, key);                                                     \
  const Func* func = m_fp->m_func;                                            \
  Id id;                                                                      \
  if (mapGet(func->pnameMap(), name, &id)) {                                  \
    (val) = frame_local(m_fp, id);                                            \
  } else {                                                                    \
    ASSERT(!m_fp->hasInvName());                                              \
    if (m_fp->m_varEnv != NULL) {                                             \
      (val) = m_fp->m_varEnv->lookup(name);                                   \
    } else {                                                                  \
      (val) = NULL;                                                           \
    }                                                                         \
  }                                                                           \
} while (0)
#define LOOKUPD_VAR(name, key, val) do {                                      \
  LOOKUP_NAME(name, key);                                                     \
  const Func* func = m_fp->m_func;                                            \
  Id id;                                                                      \
  if (mapGet(func->pnameMap(), name, &id)) {                                  \
    (val) = frame_local(m_fp, id);                                            \
  } else {                                                                    \
    ASSERT(!m_fp->hasInvName());                                              \
    if (m_fp->m_varEnv == NULL) {                                             \
      m_fp->m_varEnv = VarEnv::createLazyAttach(m_fp);                        \
    }                                                                         \
    (val) = m_fp->m_varEnv->lookup(name);                                     \
    if ((val) == NULL) {                                                      \
      TypedValue tv;                                                          \
      TV_WRITE_NULL(&tv);                                                     \
      m_fp->m_varEnv->set(name, &tv);                                         \
      (val) = m_fp->m_varEnv->lookup(name);                                   \
    }                                                                         \
  }                                                                           \
} while (0)
#define LOOKUP_GBL(name, key, val) do {                                       \
  LOOKUP_NAME(name, key);                                                     \
  ASSERT(!g_vmContext->m_varEnvs.empty());                                    \
  VarEnv* varEnv = g_vmContext->m_varEnvs.front();                            \
  ASSERT(varEnv != NULL);                                                     \
  (val) = varEnv->lookup(name);                                               \
} while (0)
#define LOOKUPD_GBL(name, key, val) do {                                      \
  LOOKUP_NAME(name, key);                                                     \
  ASSERT(!g_vmContext->m_varEnvs.empty());                                    \
  VarEnv* varEnv = g_vmContext->m_varEnvs.front();                            \
  ASSERT(varEnv != NULL);                                                     \
  (val) = varEnv->lookup(name);                                               \
  if ((val) == NULL) {                                                        \
    TypedValue tv;                                                            \
    TV_WRITE_NULL(&tv);                                                       \
    varEnv->set(name, &tv);                                                   \
    (val) = varEnv->lookup(name);                                             \
  }                                                                           \
} while (0)

#define LOOKUP_SPROP(clsRef, name, key, val, visible, accessible) do {        \
  ASSERT(clsRef->m_type == KindOfClass);                                      \
  LOOKUP_NAME(name, key);                                                     \
  Class* ctx = arGetContextClass(m_fp);                                       \
  val = clsRef->m_data.pcls->getSProp(ctx, name, visible, accessible);        \
} while (0)

static void lookupClsRef(TypedValue* input,
                         TypedValue* output,
                         bool decRef = false) {
  const Class* class_ = NULL;
  if (IS_STRING_TYPE(input->m_type)) {
    class_ = Unit::loadClass(input->m_data.pstr);
    if (class_ == NULL) {
      raise_error(Strings::UNKNOWN_CLASS, input->m_data.pstr->data());
    }
  } else if (input->m_type == KindOfObject) {
    class_ = input->m_data.pobj->getVMClass();
  } else {
    raise_error("Cls: Expected string or object");
  }
  if (decRef) {
    tvRefcountedDecRef(input);
  }
  output->m_data.pcls = const_cast<Class*>(class_);
  output->_count = 0;
  output->m_type = KindOfClass;
}

int innerCount(const TypedValue* tv) {
  if (IS_REFCOUNTED_TYPE(tv->m_type)) {
    return tv->m_data.ptv->_count;
  }
  return -1;
}

static inline void ratchetRefs(TypedValue*& result, TypedValue& tvRef,
                               TypedValue& tvRef2) {
  TRACE(5, "Ratchet: result %p(k%d c%d), ref %p(k%d c%d) ref2 %p(k%d c%d)\n",
        result, result->m_type, innerCount(result),
        &tvRef, tvRef.m_type, innerCount(&tvRef),
        &tvRef2, tvRef2.m_type, innerCount(&tvRef2));
  // Due to complications associated with ArrayAccess, it is possible to acquire
  // a reference as a side effect of vector operation processing. Such a
  // reference must be retained until after the next iteration is complete.
  // Therefore, move the reference from tvRef to tvRef2, so that the reference
  // will be released one iteration later. But only do this if tvRef was used in
  // this iteration, otherwise we may wipe out the last reference to something
  // that we need to stay alive until the next iteration.
  if (tvRef.m_type != KindOfUninit) {
    if (IS_REFCOUNTED_TYPE(tvRef2.m_type)) {
      tvDecRef(&tvRef2);
      TRACE(5, "Ratchet: decref tvref2\n");
      tvWriteUninit(&tvRef2);
    }

    memcpy(&tvRef2, &tvRef, sizeof(TypedValue));
    tvWriteUninit(&tvRef);
    // Update result to point to relocated reference. This can be done
    // unconditionally here because we maintain the invariant throughout that
    // either tvRef is KindOfUninit, or tvRef contains a valid object that
    // result points to.
    ASSERT(result == &tvRef);
    result = &tvRef2;
  }
}

#define DECLARE_GETHELPER_ARGS                  \
  unsigned ndiscard;                            \
  TypedValue* tvRet;                            \
  TypedValue* base;                             \
  bool baseStrOff = false;                      \
  TypedValue tvScratch;                         \
  TypedValue tvRef;                             \
  TypedValue tvRef2;                            \
  MemberCode mcode = MEL;                       \
  TypedValue* curMember = 0;
#define GETHELPERPRE_ARGS                                                     \
  pc, ndiscard, base, baseStrOff, tvScratch, tvRef, tvRef2, mcode, curMember
// The following arguments are outputs:
// pc:         bytecode instruction after the vector instruction
// ndiscard:   number of stack elements to discard
// base:       ultimate result of the vector-get
// baseStrOff: StrOff flag associated with base
// tvScratch:  temporary result storage
// tvRef:      temporary result storage
// tvRef2:     temporary result storage
// mcode:      output MemberCode for the last member if LeaveLast
// curMember:  output last member value one if LeaveLast; but undefined
//             if the last mcode == MW
//
// If saveResult is true, then upon completion of getHelperPre(),
// tvScratch contains a reference to the result (a duplicate of what
// base refers to).  getHelperPost<true>(...)  then saves the result
// to its final location.
template <bool warn,
          bool saveResult,
          VMExecutionContext::VectorLeaveCode mleave>
inline void OPTBLD_INLINE VMExecutionContext::getHelperPre(
    PC& pc,
    unsigned& ndiscard,
    TypedValue*& base,
    bool& baseStrOff,
    TypedValue& tvScratch,
    TypedValue& tvRef,
    TypedValue& tvRef2,
    MemberCode& mcode,
    TypedValue*& curMember) {
  // The caller is responsible for moving pc to point to the vector immediate
  // before calling getHelperPre().
  const ImmVector immVec = ImmVector::createFromStream(pc);
  const uint8_t* vec = immVec.vec();
  ASSERT(immVec.size() > 0);

  // PC needs to be advanced before we do anything, otherwise if we
  // raise a notice in the middle of this we could resume at the wrong
  // instruction.
  pc += immVec.size() + sizeof(int32_t) + sizeof(int32_t);

  ndiscard = immVec.numStackValues();
  int depth = ndiscard - 1;
  const LocationCode lcode = LocationCode(*vec++);

  TypedValue* loc = NULL;
  TypedValue dummy;
  Class* const ctx = arGetContextClass(m_fp);

  StringData* name;
  TypedValue* fr = NULL;
  TypedValue* cref;
  TypedValue* pname;

  switch (lcode) {
  case LNL:
    loc = frame_local_inner(m_fp, decodeVariableSizeImm(&vec));
    goto lcodeName;
  case LNC:
    loc = m_stack.indTV(depth--);
    goto lcodeName;

  lcodeName:
    LOOKUP_VAR(name, loc, fr);
    if (fr == NULL) {
      if (warn) {
        raise_notice(Strings::UNDEFINED_VARIABLE, name->data());
      }
      TV_WRITE_NULL(&dummy);
      loc = &dummy;
    } else {
      loc = fr;
    }
    LITSTR_DECREF(name);
    break;

  case LGL:
    loc = frame_local_inner(m_fp, decodeVariableSizeImm(&vec));
    goto lcodeGlobal;
  case LGC:
    loc = m_stack.indTV(depth--);
    goto lcodeGlobal;

  lcodeGlobal:
    LOOKUP_GBL(name, loc, fr);
    if (fr == NULL) {
      if (warn) {
        raise_notice(Strings::UNDEFINED_VARIABLE, name->data());
      }
      TV_WRITE_NULL(&dummy);
      loc = &dummy;
    } else {
      loc = fr;
    }
    LITSTR_DECREF(name);
    break;

  case LSC:
    cref = m_stack.topTV();
    pname = m_stack.indTV(depth--);
    goto lcodeSprop;
  case LSL:
    cref = m_stack.topTV();
    pname = frame_local_inner(m_fp, decodeVariableSizeImm(&vec));
    goto lcodeSprop;

  lcodeSprop: {
    bool visible, accessible;
    ASSERT(cref->m_type == KindOfClass);
    const Class* class_ = cref->m_data.pcls;
    StringData* name;
    LOOKUP_NAME(name, pname);
    loc = class_->getSProp(ctx, name, visible, accessible);
    if (!(visible && accessible)) {
      raise_error("Invalid static property access: %s::%s",
                  class_->name()->data(),
                  name->data());
    }
    LITSTR_DECREF(name);
    break;
  }

  case LL:
    loc = frame_local_inner(m_fp, decodeVariableSizeImm(&vec));
    break;
  case LC:
  case LR:
    loc = m_stack.indTV(depth--);
    break;
  default: ASSERT(false);
  }

  base = loc;
  tvWriteUninit(&tvScratch);
  tvWriteUninit(&tvRef);
  tvWriteUninit(&tvRef2);

  // Iterate through the members.
  while (vec < pc) {
    mcode = MemberCode(*vec++);
    curMember = memberCodeHasImm(mcode)
      ? frame_local_inner(m_fp, decodeVariableSizeImm(&vec))
      : m_stack.indTV(depth--);

    if (mleave == LeaveLast) {
      if (vec >= pc) {
        ASSERT(vec == pc);
        break;
      }
    }

    TypedValue* result;
    switch (mcode) {
    case MEL:
    case MEC:
      result = Elem<warn>(tvScratch, tvRef, base, baseStrOff, curMember);
      break;
    case MPL:
    case MPC:
      result = prop<warn, false, false>(tvScratch, tvRef, ctx, base,
                                        curMember);
      break;
    case MW:
      raise_error("Cannot use [] for reading");
      result = NULL;
      break;
    default:
      ASSERT(false);
      result = NULL; // Silence compiler warning.
    }
    ASSERT(result != NULL);
    ratchetRefs(result, tvRef, tvRef2);
    base = result;
  }

  if (mleave == ConsumeAll) {
    ASSERT(vec == pc);
    if (debug) {
      if (lcode == LSC || lcode == LSL) {
        ASSERT(depth == 0);
      } else {
        ASSERT(depth == -1);
      }
    }
  }

  if (saveResult) {
    // If requested, save a copy of the result.  If base already points to
    // tvScratch, no reference counting is necessary, because (with the
    // exception of the following block), tvScratch is never populated such
    // that it owns a reference that must be accounted for.
    if (base != &tvScratch) {
      // Acquire a reference to the result via tvDup(); base points to the
      // result but does not own a reference.
      tvDup(base, &tvScratch);
    }
  }
}

#define GETHELPERPOST_ARGS ndiscard, tvRet, tvScratch, tvRef, tvRef2
template <bool saveResult>
inline void OPTBLD_INLINE VMExecutionContext::getHelperPost(
    unsigned ndiscard, TypedValue*& tvRet, TypedValue& tvScratch,
    TypedValue& tvRef, TypedValue& tvRef2) {
  // Clean up all ndiscard elements on the stack.  Actually discard
  // only ndiscard - 1, and overwrite the last cell with the result,
  // or if ndiscard is zero we actually need to allocate a cell.
  for (unsigned depth = 0; depth < ndiscard; ++depth) {
    TypedValue* tv = m_stack.indTV(depth);
    tvRefcountedDecRef(tv);
  }

  if (!ndiscard) {
    tvRet = m_stack.allocTV();
  } else {
    m_stack.ndiscard(ndiscard - 1);
    tvRet = m_stack.topTV();
  }
  tvRefcountedDecRef(&tvRef);
  tvRefcountedDecRef(&tvRef2);

  if (saveResult) {
    // If tvRef wasn't just allocated, we've already decref'd it in
    // the loop above.
    memcpy(tvRet, &tvScratch, sizeof(TypedValue));
  }
}

#define GETHELPER_ARGS \
  pc, ndiscard, tvRet, base, baseStrOff, tvScratch, tvRef, tvRef2,  \
  mcode, curMember
inline void OPTBLD_INLINE
VMExecutionContext::getHelper(PC& pc,
                              unsigned& ndiscard,
                              TypedValue*& tvRet,
                              TypedValue*& base,
                              bool& baseStrOff,
                              TypedValue& tvScratch,
                              TypedValue& tvRef,
                              TypedValue& tvRef2,
                              MemberCode& mcode,
                              TypedValue*& curMember) {
  getHelperPre<true, true, ConsumeAll>(GETHELPERPRE_ARGS);
  getHelperPost<true>(GETHELPERPOST_ARGS);
}

void
VMExecutionContext::getElem(TypedValue* base, TypedValue* key,
                            TypedValue* dest) {
  ASSERT(base->m_type != KindOfArray);
  bool baseStrOff = false;
  VMRegAnchor _;
  EXCEPTION_GATE_ENTER();
  TV_WRITE_UNINIT(dest);
  TypedValue* result = Elem<true>(*dest, *dest, base, baseStrOff, key);
  if (result != dest) {
    tvDup(result, dest);
  }
  EXCEPTION_GATE_LEAVE();
}

#define DECLARE_SETHELPER_ARGS                                                \
  unsigned ndiscard;                                                          \
  TypedValue* base;                                                           \
  bool baseStrOff = false;                                                    \
  TypedValue tvScratch;                                                       \
  TypedValue tvRef;                                                           \
  TypedValue tvRef2;                                                          \
  MemberCode mcode = MEL;                                                     \
  TypedValue* curMember = 0;
#define SETHELPERPRE_ARGS                                                     \
  pc, ndiscard, base, baseStrOff, tvScratch, tvRef, tvRef2, mcode, curMember
// The following arguments are outputs:  (TODO put them in struct)
// pc:         bytecode instruction after the vector instruction
// ndiscard:   number of stack elements to discard
// base:       ultimate result of the vector-get
// baseStrOff: StrOff flag associated with base
// tvScratch:  temporary result storage
// tvRef:      temporary result storage
// tvRef2:     temporary result storage
// mcode:      output MemberCode for the last member if LeaveLast
// curMember:  output last member value one if LeaveLast; but undefined
//             if the last mcode == MW
//
// TODO(#1068709) XXX this function should be merged with getHelperPre.
template <bool warn,
          bool define,
          bool unset,
          unsigned mdepth, // extra args on stack for set (e.g. rhs)
          VMExecutionContext::VectorLeaveCode mleave>
inline bool OPTBLD_INLINE VMExecutionContext::setHelperPre(
    PC& pc, unsigned& ndiscard, TypedValue*& base,
    bool& baseStrOff, TypedValue& tvScratch, TypedValue& tvRef,
    TypedValue& tvRef2, MemberCode& mcode, TypedValue*& curMember) {
  // The caller must move pc to the vector immediate before calling
  // setHelperPre.
  const ImmVector immVec = ImmVector::createFromStream(pc);
  const uint8_t* vec = immVec.vec();
  ASSERT(immVec.size() > 0);

  // PC needs to be advanced before we do anything, otherwise if we
  // raise a notice in the middle of this we could resume at the wrong
  // instruction.
  pc += immVec.size() + sizeof(int32_t) + sizeof(int32_t);

  ndiscard = immVec.numStackValues();
  int depth = mdepth + ndiscard - 1;
  const LocationCode lcode = LocationCode(*vec++);

  TypedValue* loc = NULL;
  TypedValue dummy;
  Class* const ctx = arGetContextClass(m_fp);

  StringData* name;
  TypedValue* fr = NULL;
  TypedValue* cref;
  TypedValue* pname;

  switch (lcode) {
  case LNL:
    loc = frame_local_inner(m_fp, decodeVariableSizeImm(&vec));
    goto lcodeName;
  case LNC:
    loc = m_stack.indTV(depth--);
    goto lcodeName;

  lcodeName:
    if (define) {
      LOOKUPD_VAR(name, loc, fr);
    } else {
      LOOKUP_VAR(name, loc, fr);
    }
    if (fr == NULL) {
      if (warn) {
        raise_notice(Strings::UNDEFINED_VARIABLE, name->data());
      }
      TV_WRITE_NULL(&dummy);
      loc = &dummy;
    } else {
      loc = fr;
    }
    LITSTR_DECREF(name);
    break;

  case LGL:
    loc = frame_local_inner(m_fp, decodeVariableSizeImm(&vec));
    goto lcodeGlobal;
  case LGC:
    loc = m_stack.indTV(depth--);
    goto lcodeGlobal;

  lcodeGlobal:
    if (define) {
      LOOKUPD_GBL(name, loc, fr);
    } else {
      LOOKUP_GBL(name, loc, fr);
    }
    if (fr == NULL) {
      if (warn) {
        raise_notice(Strings::UNDEFINED_VARIABLE, name->data());
      }
      TV_WRITE_NULL(&dummy);
      loc = &dummy;
    } else {
      loc = fr;
    }
    LITSTR_DECREF(name);
    break;

  case LSC:
    cref = m_stack.indTV(mdepth);
    pname = m_stack.indTV(depth--);
    goto lcodeSprop;
  case LSL:
    cref = m_stack.indTV(mdepth);
    pname = frame_local_inner(m_fp, decodeVariableSizeImm(&vec));
    goto lcodeSprop;

  lcodeSprop: {
    bool visible, accessible;
    ASSERT(cref->m_type == KindOfClass);
    const Class* class_ = cref->m_data.pcls;
    StringData* name;
    LOOKUP_NAME(name, pname);
    loc = class_->getSProp(ctx, name, visible, accessible);
    if (!(visible && accessible)) {
      raise_error("Invalid static property access: %s::%s",
                  class_->name()->data(),
                  name->data());
    }
    LITSTR_DECREF(name);
    break;
  }

  case LL:
    loc = frame_local_inner(m_fp, decodeVariableSizeImm(&vec));
    break;
  case LC:
  case LR:
    loc = m_stack.indTV(depth--);
    break;
  default: ASSERT(false);
  }

  base = loc;
  tvWriteUninit(&tvScratch);
  tvWriteUninit(&tvRef);
  tvWriteUninit(&tvRef2);

  // Iterate through the members.
  while (vec < pc) {
    mcode = MemberCode(*vec++);
    curMember = memberCodeHasImm(mcode) ?
      frame_local_inner(m_fp, decodeVariableSizeImm(&vec)) :
      mcode == MW ? NULL :
      m_stack.indTV(depth--);

    if (mleave == LeaveLast) {
      if (vec >= pc) {
        ASSERT(vec == pc);
        break;
      }
    }

    TypedValue* result;
    switch (mcode) {
    case MEL:
    case MEC:
      if (unset) {
        result = ElemU(tvScratch, tvRef, base, curMember);
      } else if (define) {
        result = ElemD<warn>(tvScratch, tvRef, base, curMember);
      } else {
        result = Elem<warn>(tvScratch, tvRef, base, baseStrOff, curMember);
      }
      break;
    case MPL:
    case MPC:
      result = prop<warn, define, unset>(tvScratch, tvRef, ctx, base,
                                         curMember);
      break;
    case MW:
      ASSERT(define);
      result = NewElem(tvScratch, tvRef, base);
      break;
    default:
      ASSERT(false);
      result = NULL; // Silence compiler warning.
    }
    ASSERT(result != NULL);
    ratchetRefs(result, tvRef, tvRef2);
    // Check whether an error occurred (i.e. no result was set).
    if (result == &tvScratch && result->m_type == KindOfUninit) {
      return true;
    }
    base = result;
  }

  if (mleave == ConsumeAll) {
    ASSERT(vec == pc);
    if (debug) {
      if (lcode == LSC || lcode == LSL) {
        ASSERT(depth == int(mdepth));
      } else {
        ASSERT(depth == int(mdepth) - 1);
      }
    }
  }

  return false;
}

#define SETHELPERPOST_ARGS ndiscard, tvRef, tvRef2
template <unsigned mdepth>
inline void OPTBLD_INLINE VMExecutionContext::setHelperPost(
    unsigned ndiscard, TypedValue& tvRef, TypedValue& tvRef2) {
  // Clean up the stack.  Decref all the elements for the vector, but
  // leave the first mdepth (they are not part of the vector data).
  for (unsigned depth = mdepth; depth-mdepth < ndiscard; ++depth) {
    TypedValue* tv = m_stack.indTV(depth);
    tvRefcountedDecRef(tv);
  }

  // NOTE: currently the only instructions using this that have return
  // values on the stack also have more inputs than the K-vector, so
  // mdepth > 0.  They also always return the original top value of
  // the stack.
  if (mdepth > 0) {
    ASSERT(mdepth == 1 &&
      "We don't really support mdepth > 1 in setHelperPost");

    TypedValue* retSrc = m_stack.topTV();
    if (ndiscard > 0) {
      TypedValue* dest = m_stack.indTV(ndiscard + mdepth - 1);
      memcpy(dest, retSrc, sizeof *dest);
    }
  }

  m_stack.ndiscard(ndiscard);
  tvRefcountedDecRef(&tvRef);
  tvRefcountedDecRef(&tvRef2);
}

inline void OPTBLD_INLINE VMExecutionContext::iopLowInvalid(PC& pc) {
  fprintf(stderr, "invalid bytecode executed\n");
  abort();
}

inline void OPTBLD_INLINE VMExecutionContext::iopNop(PC& pc) {
  NEXT();
}

inline void OPTBLD_INLINE VMExecutionContext::iopPopC(PC& pc) {
  NEXT();
  m_stack.popC();
}

inline void OPTBLD_INLINE VMExecutionContext::iopPopV(PC& pc) {
  NEXT();
  m_stack.popV();
}

inline void OPTBLD_INLINE VMExecutionContext::iopPopR(PC& pc) {
  NEXT();
  if (m_stack.topTV()->m_type != KindOfVariant) {
    m_stack.popC();
  } else {
    m_stack.popV();
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopDup(PC& pc) {
  NEXT();
  m_stack.dup();
}

inline void OPTBLD_INLINE VMExecutionContext::iopBox(PC& pc) {
  NEXT();
  m_stack.box();
}

inline void OPTBLD_INLINE VMExecutionContext::iopUnbox(PC& pc) {
  NEXT();
  m_stack.unbox();
}

inline void OPTBLD_INLINE VMExecutionContext::iopBoxR(PC& pc) {
  NEXT();
  TypedValue* tv = m_stack.topTV();
  if (tv->m_type != KindOfVariant) {
    tvBox(tv);
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopUnboxR(PC& pc) {
  NEXT();
  if (m_stack.topTV()->m_type == KindOfVariant) {
    m_stack.unbox();
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopNull(PC& pc) {
  NEXT();
  m_stack.pushNull();
}

inline void OPTBLD_INLINE VMExecutionContext::iopTrue(PC& pc) {
  NEXT();
  m_stack.pushTrue();
}

inline void OPTBLD_INLINE VMExecutionContext::iopFalse(PC& pc) {
  NEXT();
  m_stack.pushFalse();
}

inline void OPTBLD_INLINE VMExecutionContext::iopFile(PC& pc) {
  NEXT();
  const StringData* s = m_fp->m_func->unit()->filepath();
  m_stack.pushStaticString(const_cast<StringData*>(s));
}

inline void OPTBLD_INLINE VMExecutionContext::iopDir(PC& pc) {
  NEXT();
  const StringData* s = m_fp->m_func->unit()->dirpath();
  m_stack.pushStaticString(const_cast<StringData*>(s));
}

inline void OPTBLD_INLINE VMExecutionContext::iopInt(PC& pc) {
  NEXT();
  DECODE(int64, i);
  m_stack.pushInt(i);
}

inline void OPTBLD_INLINE VMExecutionContext::iopDouble(PC& pc) {
  NEXT();
  DECODE(double, d);
  m_stack.pushDouble(d);
}

inline void OPTBLD_INLINE VMExecutionContext::iopString(PC& pc) {
  NEXT();
  DECODE_LITSTR(s);
  m_stack.pushStaticString(s);
}

inline void OPTBLD_INLINE VMExecutionContext::iopArray(PC& pc) {
  NEXT();
  DECODE(Id, id);
  ArrayData* a = m_fp->m_func->unit()->lookupArrayId(id);
  m_stack.pushStaticArray(a);
}

inline void OPTBLD_INLINE VMExecutionContext::iopNewArray(PC& pc) {
  NEXT();
  // Clever sizing avoids extra work in HphpArray construction.
  ArrayData* arr = NEW(HphpArray)(size_t(3U) << (HphpArray::MinLgTableSize-2));
  m_stack.pushArray(arr);
}

inline void OPTBLD_INLINE VMExecutionContext::iopAddElemC(PC& pc) {
  NEXT();
  Cell* c1 = m_stack.topC();
  Cell* c2 = m_stack.indC(1);
  Cell* c3 = m_stack.indC(2);
  if (c3->m_type != KindOfArray) {
    raise_error("AddElemC: $3 must be an array");
  }
  if (c2->m_type == KindOfInt64) {
    tvCellAsVariant(c3).asArrRef().set(c2->m_data.num, tvAsCVarRef(c1));
  } else {
    tvCellAsVariant(c3).asArrRef().set(tvAsCVarRef(c2), tvAsCVarRef(c1));
  }
  m_stack.popC();
  m_stack.popC();
}

inline void OPTBLD_INLINE VMExecutionContext::iopAddElemV(PC& pc) {
  NEXT();
  Var* v1 = m_stack.topV();
  Cell* c2 = m_stack.indC(1);
  Cell* c3 = m_stack.indC(2);
  if (c3->m_type != KindOfArray) {
    raise_error("AddElemC: $3 must be an array");
  }
  if (c2->m_type == KindOfInt64) {
    tvCellAsVariant(c3).asArrRef().set(c2->m_data.num, ref(tvAsCVarRef(v1)));
  } else {
    tvCellAsVariant(c3).asArrRef().set(tvAsCVarRef(c2), ref(tvAsCVarRef(v1)));
  }
  m_stack.popV();
  m_stack.popC();
}

inline void OPTBLD_INLINE VMExecutionContext::iopAddNewElemC(PC& pc) {
  NEXT();
  Cell* c1 = m_stack.topC();
  Cell* c2 = m_stack.indC(1);
  if (c2->m_type != KindOfArray) {
    raise_error("AddNewElemC: $2 must be an array");
  }
  tvCellAsVariant(c2).asArrRef().append(tvAsCVarRef(c1));
  m_stack.popC();
}

inline void OPTBLD_INLINE VMExecutionContext::iopAddNewElemV(PC& pc) {
  NEXT();
  Var* v1 = m_stack.topV();
  Cell* c2 = m_stack.indC(1);
  if (c2->m_type != KindOfArray) {
    raise_error("AddNewElemC: $2 must be an array");
  }
  tvCellAsVariant(c2).asArrRef().append(ref(tvAsCVarRef(v1)));
  m_stack.popV();
}

inline void OPTBLD_INLINE VMExecutionContext::iopCns(PC& pc) {
  NEXT();
  DECODE_LITSTR(s);
  TypedValue* cns = getCns(s);
  if (cns != NULL) {
    Cell* c1 = m_stack.allocC();
    TV_READ_CELL(cns, c1);
  } else {
    raise_notice(Strings::UNDEFINED_CONSTANT,
                 s->data(), s->data());
    m_stack.pushStaticString(s);
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopDefCns(PC& pc) {
  NEXT();
  DECODE_LITSTR(s);
  TypedValue* tv = m_stack.topTV();

  tvAsVariant(tv) = setCns(s, tvAsCVarRef(tv));
}

inline void OPTBLD_INLINE VMExecutionContext::iopClsCns(PC& pc) {
  NEXT();
  DECODE_LITSTR(clsCnsName);
  TypedValue* tv = m_stack.topTV();
  ASSERT(tv->m_type == KindOfClass);
  Class* class_ = tv->m_data.pcls;
  ASSERT(class_ != NULL);
  TypedValue* clsCns = class_->clsCnsGet(clsCnsName);
  if (clsCns == NULL) {
    raise_error("Couldn't find constant %s::%s",
                class_->name()->data(), clsCnsName->data());
  }
  TV_READ_CELL(clsCns, tv);
}

inline void OPTBLD_INLINE VMExecutionContext::iopClsCnsD(PC& pc) {
  NEXT();
  DECODE_LITSTR(clsCnsName);
  DECODE(Id, classId);
  const NamedEntityPair& classNamedEntity =
    m_fp->m_func->unit()->lookupNamedEntityPairId(classId);

  TypedValue* clsCns = lookupClsCns(classNamedEntity.second,
                                    classNamedEntity.first, clsCnsName);
  ASSERT(clsCns != NULL);
  Cell* c1 = m_stack.allocC();
  TV_READ_CELL(clsCns, c1);
}

inline void OPTBLD_INLINE VMExecutionContext::iopConcat(PC& pc) {
  NEXT();
  Cell* c1 = m_stack.topC();
  Cell* c2 = m_stack.indC(1);
  if (IS_STRING_TYPE(c1->m_type) && IS_STRING_TYPE(c2->m_type)) {
    tvCellAsVariant(c2) = concat(tvCellAsVariant(c2), tvCellAsCVarRef(c1));
  } else {
    tvCellAsVariant(c2) = concat(tvCellAsVariant(c2).toString(),
                                 tvCellAsCVarRef(c1).toString());
  }
  ASSERT(c2->m_data.ptv->_count > 0);
  m_stack.popC();
}

#define MATHOP(OP, VOP) do {                                                  \
  NEXT();                                                                     \
  Cell* c1 = m_stack.topC();                                                  \
  Cell* c2 = m_stack.indC(1);                                                 \
  if (c2->m_type == KindOfInt64 && c1->m_type == KindOfInt64) {               \
    int64 a = c2->m_data.num;                                                 \
    int64 b = c1->m_data.num;                                                 \
    MATHOP_DIVCHECK(0)                                                        \
    c2->m_data.num = a OP b;                                                  \
    m_stack.popX();                                                           \
  }                                                                           \
  MATHOP_DOUBLE(OP)                                                           \
  else {                                                                      \
    tvCellAsVariant(c2) = VOP(tvCellAsVariant(c2), tvCellAsCVarRef(c1));      \
    m_stack.popC();                                                           \
  }                                                                           \
} while (0)
#define MATHOP_DOUBLE(OP)                                                     \
  else if (c2->m_type == KindOfDouble                                         \
             && c1->m_type == KindOfDouble) {                                 \
    double a = c2->m_data.dbl;                                                \
    double b = c1->m_data.dbl;                                                \
    MATHOP_DIVCHECK(0.0)                                                      \
    c2->m_data.dbl = a OP b;                                                  \
    m_stack.popX();                                                           \
  }
#define MATHOP_DIVCHECK(x)
inline void OPTBLD_INLINE VMExecutionContext::iopAdd(PC& pc) {
  MATHOP(+, plus);
}

inline void OPTBLD_INLINE VMExecutionContext::iopSub(PC& pc) {
  MATHOP(-, minus);
}

inline void OPTBLD_INLINE VMExecutionContext::iopMul(PC& pc) {
  MATHOP(*, multiply);
}
#undef MATHOP_DIVCHECK

#define MATHOP_DIVCHECK(x)                                                    \
    if (b == x) {                                                             \
      raise_warning("Division by zero");                                      \
      c2->m_data.num = 0;                                                     \
      c2->m_type = KindOfBoolean;                                             \
    } else
inline void OPTBLD_INLINE VMExecutionContext::iopDiv(PC& pc) {
  NEXT();
  Cell* c1 = m_stack.topC();  // denominator
  Cell* c2 = m_stack.indC(1); // numerator
  // Special handling for evenly divisible ints
  if (c2->m_type == KindOfInt64 && c1->m_type == KindOfInt64
      && c1->m_data.num != 0 && c2->m_data.num % c1->m_data.num == 0) {
    int64 b = c1->m_data.num;
    MATHOP_DIVCHECK(0)
    c2->m_data.num /= b;
    m_stack.popX();
  }
  MATHOP_DOUBLE(/)
  else {
    tvCellAsVariant(c2) = divide(tvCellAsVariant(c2), tvCellAsCVarRef(c1));
    m_stack.popC();
  }
}
#undef MATHOP_DOUBLE

#define MATHOP_DOUBLE(OP)
inline void OPTBLD_INLINE VMExecutionContext::iopMod(PC& pc) {
  MATHOP(%, modulo);
}
#undef MATHOP_DOUBLE
#undef MATHOP_DIVCHECK

#define LOGICOP(OP) do {                                                      \
  NEXT();                                                                     \
  Cell* c1 = m_stack.topC();                                                  \
  Cell* c2 = m_stack.indC(1);                                                 \
  {                                                                           \
    tvCellAsVariant(c2) =                                                     \
      (bool)(bool(tvCellAsVariant(c2)) OP bool(tvCellAsVariant(c1)));         \
  }                                                                           \
  m_stack.popC();                                                             \
} while (0)
inline void OPTBLD_INLINE VMExecutionContext::iopAnd(PC& pc) {
  LOGICOP(&&);
}

inline void OPTBLD_INLINE VMExecutionContext::iopOr(PC& pc) {
  LOGICOP(||);
}

inline void OPTBLD_INLINE VMExecutionContext::iopXor(PC& pc) {
  LOGICOP(^);
}
#undef LOGICOP

inline void OPTBLD_INLINE VMExecutionContext::iopNot(PC& pc) {
  NEXT();
  Cell* c1 = m_stack.topC();
  tvCellAsVariant(c1) = !bool(tvCellAsVariant(c1));
}

#define CMPOP(OP, VOP) do {                                                   \
  NEXT();                                                                     \
  Cell* c1 = m_stack.topC();                                                  \
  Cell* c2 = m_stack.indC(1);                                                 \
  if (c2->m_type == KindOfInt64 && c1->m_type == KindOfInt64) {               \
    int64 a = c2->m_data.num;                                                 \
    int64 b = c1->m_data.num;                                                 \
    c2->m_data.num = (a OP b);                                                \
    c2->m_type = KindOfBoolean;                                               \
    m_stack.popX();                                                           \
  } else {                                                                    \
    int64 result = VOP(tvCellAsVariant(c2), tvCellAsCVarRef(c1));             \
    tvRefcountedDecRefCell(c2);                                               \
    c2->m_data.num = result;                                                  \
    c2->_count = 0;                                                           \
    c2->m_type = KindOfBoolean;                                               \
    m_stack.popC();                                                           \
  }                                                                           \
} while (0)
inline void OPTBLD_INLINE VMExecutionContext::iopSame(PC& pc) {
  CMPOP(==, same);
}

inline void OPTBLD_INLINE VMExecutionContext::iopNSame(PC& pc) {
  CMPOP(!=, !same);
}

inline void OPTBLD_INLINE VMExecutionContext::iopEq(PC& pc) {
  CMPOP(==, equal);
}

inline void OPTBLD_INLINE VMExecutionContext::iopNeq(PC& pc) {
  CMPOP(!=, !equal);
}

inline void OPTBLD_INLINE VMExecutionContext::iopLt(PC& pc) {
  CMPOP(<, less);
}

inline void OPTBLD_INLINE VMExecutionContext::iopLte(PC& pc) {
  CMPOP(<=, not_more);
}

inline void OPTBLD_INLINE VMExecutionContext::iopGt(PC& pc) {
  CMPOP(>, more);
}

inline void OPTBLD_INLINE VMExecutionContext::iopGte(PC& pc) {
  CMPOP(>=, not_less);
}
#undef CMPOP

#define MATHOP_DOUBLE(OP)
#define MATHOP_DIVCHECK(x)
inline void OPTBLD_INLINE VMExecutionContext::iopBitAnd(PC& pc) {
  MATHOP(&, bitwise_and);
}

inline void OPTBLD_INLINE VMExecutionContext::iopBitOr(PC& pc) {
  MATHOP(|, bitwise_or);
}

inline void OPTBLD_INLINE VMExecutionContext::iopBitXor(PC& pc) {
  MATHOP(^, bitwise_xor);
}
#undef MATHOP
#undef MATHOP_DOUBLE
#undef MATHOP_DIVCHECK

inline void OPTBLD_INLINE VMExecutionContext::iopBitNot(PC& pc) {
  NEXT();
  Cell* c1 = m_stack.topC();
  if (LIKELY(c1->m_type == KindOfInt64)) {
    c1->m_data.num = ~c1->m_data.num;
  } else if (c1->m_type == KindOfDouble) {
    c1->m_type = KindOfInt64;
    c1->m_data.num = ~int64(c1->m_data.dbl);
  } else if (IS_STRING_TYPE(c1->m_type)) {
    tvCellAsVariant(c1) = ~tvCellAsVariant(c1);
  } else if (c1->m_type == KindOfInt32) {
    // Separate from KindOfInt64 due to the infrequency of KindOfInt32.
    c1->m_data.num = ~c1->m_data.num;
  } else {
    raise_error("Unsupported operand type for ~");
  }
}

#define SHIFTOP(OP) do {                                                      \
  NEXT();                                                                     \
  Cell* c1 = m_stack.topC();                                                  \
  Cell* c2 = m_stack.indC(1);                                                 \
  if (c2->m_type == KindOfInt64 && c1->m_type == KindOfInt64) {               \
    int64 a = c2->m_data.num;                                                 \
    int64 b = c1->m_data.num;                                                 \
    c2->m_data.num = a OP b;                                                  \
    m_stack.popX();                                                           \
  } else {                                                                    \
    tvCellAsVariant(c2) = tvCellAsVariant(c2).toInt64() OP                    \
                          tvCellAsCVarRef(c1).toInt64();                      \
    m_stack.popC();                                                           \
  }                                                                           \
} while (0)
inline void OPTBLD_INLINE VMExecutionContext::iopShl(PC& pc) {
  SHIFTOP(<<);
}

inline void OPTBLD_INLINE VMExecutionContext::iopShr(PC& pc) {
  SHIFTOP(>>);
}
#undef SHIFTOP

inline void OPTBLD_INLINE VMExecutionContext::iopCastBool(PC& pc) {
  NEXT();
  Cell* c1 = m_stack.topC();
  tvCastToBooleanInPlace(c1);
}

inline void OPTBLD_INLINE VMExecutionContext::iopCastInt(PC& pc) {
  NEXT();
  Cell* c1 = m_stack.topC();
  tvCastToInt64InPlace(c1);
}

inline void OPTBLD_INLINE VMExecutionContext::iopCastDouble(PC& pc) {
  NEXT();
  Cell* c1 = m_stack.topC();
  tvCastToDoubleInPlace(c1);
}

inline void OPTBLD_INLINE VMExecutionContext::iopCastString(PC& pc) {
  NEXT();
  Cell* c1 = m_stack.topC();
  tvCastToStringInPlace(c1);
}

inline void OPTBLD_INLINE VMExecutionContext::iopCastArray(PC& pc) {
  NEXT();
  Cell* c1 = m_stack.topC();
  tvCastToArrayInPlace(c1);
}

inline void OPTBLD_INLINE VMExecutionContext::iopCastObject(PC& pc) {
  NEXT();
  Cell* c1 = m_stack.topC();
  tvCastToObjectInPlace(c1);
}

inline bool OPTBLD_INLINE VMExecutionContext::cellInstanceOf(
  TypedValue* tv, const NamedEntity* ne) {
  ASSERT(tv->m_type != KindOfVariant);
  if (tv->m_type == KindOfObject) {
    Class* cls = Unit::lookupClass(ne);
    if (cls) return tv->m_data.pobj->instanceof(cls);
  }
  return false;
}

inline void OPTBLD_INLINE VMExecutionContext::iopInstanceOf(PC& pc) {
  NEXT();
  Cell* c1 = m_stack.topC();   // c2 instanceof c1
  Cell* c2 = m_stack.indC(1);
  bool r = false;
  if (IS_STRING_TYPE(c1->m_type)) {
    const NamedEntity* rhs = Unit::GetNamedEntity(c1->m_data.pstr);
    r = cellInstanceOf(c2, rhs);
  } else if (c1->m_type == KindOfObject) {
    if (c2->m_type == KindOfObject) {
      ObjectData* lhs = c2->m_data.pobj;
      ObjectData* rhs = c1->m_data.pobj;
      r = lhs->instanceof(rhs->getVMClass());
    }
  } else {
    raise_error("Class name must be a valid object or a string");
  }
  m_stack.popC();
  tvRefcountedDecRefCell(c2);
  c2->m_data.num = r;
  c2->_count = 0;
  c2->m_type = KindOfBoolean;
}

inline void OPTBLD_INLINE VMExecutionContext::iopInstanceOfD(PC& pc) {
  NEXT();
  DECODE(Id, id);
  const NamedEntity* ne = m_fp->m_func->unit()->lookupNamedEntityId(id);
  Cell* c1 = m_stack.topC();
  bool r = cellInstanceOf(c1, ne);
  tvRefcountedDecRefCell(c1);
  c1->m_data.num = r;
  c1->_count = 0;
  c1->m_type = KindOfBoolean;
}

inline void OPTBLD_INLINE VMExecutionContext::iopPrint(PC& pc) {
  NEXT();
  Cell* c1 = m_stack.topC();
  print(tvCellAsVariant(c1).toString());
  tvRefcountedDecRefCell(c1);
  c1->_count = 0;
  c1->m_type = KindOfInt64;
  c1->m_data.num = 1;
}

inline void OPTBLD_INLINE VMExecutionContext::iopClone(PC& pc) {
  NEXT();
  TypedValue* tv = m_stack.topTV();
  if (tv->m_type != KindOfObject) {
    raise_error("clone called on non-object");
  }
  ObjectData* obj = tv->m_data.pobj;
  const Class* class_ UNUSED = obj->getVMClass();
  ObjectData* newobj;

  /* XXX: This only works for pure user classes or pure
   * builtins. For both, we call obj->clone(), which does
   * the correct thing.
   * For user classes that extend a builtin, we need to call
   * the builtin's ->clone() method, which is currently not
   * available.
   */
  ASSERT(class_->derivesFromBuiltin() == false);
  newobj = obj->clone();
  m_stack.popTV();
  m_stack.pushNull();
  tv->m_type = KindOfObject;
  tv->_count = 0;
  tv->m_data.pobj = newobj;
}

inline int OPTBLD_INLINE
VMExecutionContext::handleUnwind(UnwindStatus unwindType) {
  int longJumpType;
  if (unwindType == UnwindPropagate) {
    longJumpType = LONGJUMP_PROPAGATE;
    if (m_nestedVMs.empty()) {
      m_halted = true;
      m_fp = NULL;
      m_pc = NULL;
    } else {
      VMState savedVM;
      m_nestedVMMap.erase(m_firstAR);
      memcpy(&savedVM, &m_nestedVMs.back(), sizeof(savedVM));
      m_pc = savedVM.pc;
      m_fp = savedVM.fp;
      m_firstAR = savedVM.firstAR;
      ASSERT(m_stack.top() == savedVM.sp);
      popVMState();
    }
  } else {
    ASSERT(unwindType == UnwindResumeVM);
    longJumpType = LONGJUMP_RESUMEVM;
  }
  return longJumpType;
}

inline void OPTBLD_INLINE VMExecutionContext::iopExit(PC& pc) {
  NEXT();
  int exitCode = 0;
  Cell* c1 = m_stack.topC();
  if (c1->m_type == KindOfInt64) {
    exitCode = c1->m_data.num;
  } else {
    print(tvCellAsVariant(c1).toString());
  }
  m_stack.popC();
  throw ExitException(exitCode);
}

inline void OPTBLD_INLINE VMExecutionContext::iopRaise(PC& pc) {
  not_implemented();
}

inline void OPTBLD_INLINE VMExecutionContext::iopFatal(PC& pc) {
  TypedValue* top = m_stack.topTV();
  std::string msg;
  if (IS_STRING_TYPE(top->m_type)) {
    msg = top->m_data.pstr->data();
  } else {
    msg = "Fatal error message not a string";
  }
  m_stack.popTV();
  raise_error(msg);
}

#define JMP_SURPRISE_CHECK()                                            \
  if (offset < 0 && UNLIKELY(                                           \
        ThreadInfo::s_threadInfo->m_reqInjectionData.conditionFlags)) { \
    SYNC();                                                             \
    EventHook::CheckSurprise();                                         \
  }

inline void OPTBLD_INLINE VMExecutionContext::iopJmp(PC& pc) {
  NEXT();
  DECODE_JMP(Offset, offset);
  JMP_SURPRISE_CHECK();
  pc += offset - 1;
}

#define JMPOP(OP, VOP) do {                                                   \
  Cell* c1 = m_stack.topC();                                                  \
  if (c1->m_type == KindOfInt64 || c1->m_type == KindOfBoolean) {             \
    int64 n = c1->m_data.num;                                                 \
    if (n OP 0) {                                                             \
      NEXT();                                                                 \
      DECODE_JMP(Offset, offset);                                             \
      JMP_SURPRISE_CHECK();                                                   \
      pc += offset - 1;                                                       \
      m_stack.popX();                                                         \
    } else {                                                                  \
      pc += 1 + sizeof(Offset);                                               \
      m_stack.popX();                                                         \
    }                                                                         \
  } else {                                                                    \
    if (VOP(tvCellAsCVarRef(c1))) {                                           \
      NEXT();                                                                 \
      DECODE_JMP(Offset, offset);                                             \
      JMP_SURPRISE_CHECK();                                                   \
      pc += offset - 1;                                                       \
      m_stack.popC();                                                         \
    } else {                                                                  \
      pc += 1 + sizeof(Offset);                                               \
      m_stack.popC();                                                         \
    }                                                                         \
  }                                                                           \
} while (0)
inline void OPTBLD_INLINE VMExecutionContext::iopJmpZ(PC& pc) {
  JMPOP(==, !bool);
}

inline void OPTBLD_INLINE VMExecutionContext::iopJmpNZ(PC& pc) {
  JMPOP(!=, bool);
}
#undef JMPOP
#undef JMP_SURPRISE_CHECK

inline void OPTBLD_INLINE VMExecutionContext::iopSwitch(PC& pc) {
  NEXT();
  DECODE(int32_t, veclen);
  ASSERT(veclen > 0);
  TypedValue* labelTV = m_stack.topTV();
  ASSERT(labelTV->m_type == KindOfInt64);
  int64 label = labelTV->m_data.num;
  m_stack.popX();
  ASSERT(label >= 0 && label < veclen);
  Offset* jmptab = (Offset*)pc;
  pc += jmptab[label] - sizeof(Opcode) - sizeof(int32_t);
}

inline void OPTBLD_INLINE VMExecutionContext::iopRetC(PC& pc) {
  NEXT();
  uint soff = m_fp->m_soff;
  // Call the runtime helpers to free the local variables and iterators
  frame_free_locals_inl(m_fp, m_fp->m_func->numLocals());
  ActRec* sfp = arGetSfp(m_fp);
  // Memcpy the the return value on top of the activation record. This works
  // the same regardless of whether the return value is boxed or not.
  memcpy(&(m_fp->m_r), m_stack.topTV(), sizeof(TypedValue));
  // Adjust the stack
  m_stack.ndiscard(m_fp->m_func->numSlotsInFrame() + 1);

  if (LIKELY(sfp != m_fp)) {
    // Restore caller's execution state.
    m_fp = sfp;
    pc = m_fp->m_func->unit()->entry() + m_fp->m_func->base() + soff;
    m_stack.ret();
  } else {
    // No caller; terminate.
    m_stack.ret();
#ifdef HPHP_TRACE
    {
      std::ostringstream os;
      m_stack.toStringElm(os, m_stack.topTV(), m_fp);
      ONTRACE(1,
              Trace::trace("Return %s from VMExecutionContext::dispatch("
                           "%p)\n", os.str().c_str(), m_fp));
    }
#endif
    pc = NULL;
    m_fp = NULL;
    g_vmContext->m_halted = true;
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopRetV(PC& pc) {
  iopRetC(pc);
}

inline void OPTBLD_INLINE VMExecutionContext::iopUnwind(PC& pc) {
  Offset faultPC = m_fp->m_func->findFaultPCFromEH(pcOff());
  Fault fault = m_faults.back();
  UnwindStatus unwindType = m_stack.unwindFrame(m_fp, faultPC, m_pc, fault);
  hhvmThrow(handleUnwind(unwindType));
}

inline void OPTBLD_INLINE VMExecutionContext::iopThrow(PC& pc) {
  Cell* c1 = m_stack.topC();
  if (c1->m_type != KindOfObject ||
      !static_cast<Instance*>(c1->m_data.pobj)->
        instanceof(SystemLib::s_ExceptionClass)) {
    raise_error("Exceptions must be valid objects derived from the "
                "Exception base class");
  }
  ObjectData* e = c1->m_data.pobj;
  Fault fault;
  fault.m_faultType = Fault::KindOfUserException;
  fault.m_userException = e;
  m_faults.push_back(fault);
  m_stack.discard();
  DEBUGGER_ATTACHED_ONLY(phpExceptionHook(e));
  UnwindStatus unwindType = m_stack.unwindFrame(m_fp, pcOff(),
                                                m_pc, fault);
  hhvmThrow(handleUnwind(unwindType));
}

inline void OPTBLD_INLINE VMExecutionContext::iopAGetC(PC& pc) {
  NEXT();
  TypedValue* tv = m_stack.topTV();
  lookupClsRef(tv, tv, true);
}

inline void OPTBLD_INLINE VMExecutionContext::iopAGetL(PC& pc) {
  NEXT();
  DECODE_HA(local);
  TypedValue* top = m_stack.allocTV();
  TypedValue* fr = frame_local_inner(m_fp, local);
  lookupClsRef(fr, top);
}

#define RAISE_UNDEFINED_LOCAL(local) do {                               \
  size_t pind = ((uintptr_t(m_fp) - uintptr_t(local)) / sizeof(TypedValue)) \
                - 1;                                                    \
  ASSERT(pind < m_fp->m_func->pnames().size());                         \
  raise_notice(Strings::UNDEFINED_VARIABLE,                             \
               m_fp->m_func->pnames()[pind]->data());                   \
} while (0)

#define CGETH_BODY() do {                       \
  if (fr->m_type == KindOfUninit) {             \
    RAISE_UNDEFINED_LOCAL(fr);                  \
    TV_WRITE_NULL(to);                          \
  } else {                                      \
    tvDup(fr, to);                              \
    if (to->m_type == KindOfVariant) {          \
      tvUnbox(to);                              \
    }                                           \
  }                                             \
} while (0)

inline void OPTBLD_INLINE VMExecutionContext::iopCGetL(PC& pc) {
  NEXT();
  DECODE_HA(local);
  Cell* to = m_stack.allocC();
  TypedValue* fr = frame_local(m_fp, local);
  CGETH_BODY();
}

inline void OPTBLD_INLINE VMExecutionContext::iopCGetL2(PC& pc) {
  NEXT();
  DECODE_HA(local);
  TypedValue* oldTop = m_stack.topTV();
  TypedValue* newTop = m_stack.allocTV();
  memcpy(newTop, oldTop, sizeof *newTop);
  Cell* to = oldTop;
  TypedValue* fr = frame_local(m_fp, local);
  CGETH_BODY();
}

inline void OPTBLD_INLINE VMExecutionContext::iopCGetL3(PC& pc) {
  NEXT();
  DECODE_HA(local);
  TypedValue* oldTop = m_stack.topTV();
  TypedValue* oldSubTop = m_stack.indTV(1);
  TypedValue* newTop = m_stack.allocTV();
  memmove(newTop, oldTop, sizeof *oldTop * 2);
  Cell* to = oldSubTop;
  TypedValue* fr = frame_local(m_fp, local);
  CGETH_BODY();
}

inline void OPTBLD_INLINE VMExecutionContext::iopCGetN(PC& pc) {
  NEXT();
  StringData* name;
  TypedValue* to = m_stack.topTV();
  TypedValue* fr = NULL;
  LOOKUP_VAR(name, to, fr);
  if (fr == NULL) {
    raise_notice(Strings::UNDEFINED_VARIABLE, name->data());
    tvRefcountedDecRefCell(to);
    TV_WRITE_NULL(to);
  } else {
    tvRefcountedDecRefCell(to);
    CGETH_BODY();
  }
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopCGetG(PC& pc) {
  NEXT();
  StringData* name;
  TypedValue* to = m_stack.topTV();
  TypedValue* fr = NULL;
  LOOKUP_GBL(name, to, fr);
  if (fr == NULL) {
    if (MoreWarnings) {
      raise_notice(Strings::UNDEFINED_VARIABLE, name->data());
    }
    tvRefcountedDecRefCell(to);
    TV_WRITE_NULL(to);
  } else {
    tvRefcountedDecRefCell(to);
    CGETH_BODY();
  }
  LITSTR_DECREF(name);
}

#define SPROP_OP_PRELUDE                                  \
  NEXT();                                                 \
  TypedValue* clsref = m_stack.topTV();                   \
  TypedValue* nameCell = m_stack.indTV(1);                \
  TypedValue* output = nameCell;                          \
  StringData* name;                                       \
  TypedValue* val;                                        \
  bool visible, accessible;                               \
  LOOKUP_SPROP(clsref, name, nameCell, val, visible,      \
               accessible);

#define SPROP_OP_POSTLUDE                     \
  LITSTR_DECREF(name);

#define GETS(box) do {                                    \
  SPROP_OP_PRELUDE                                        \
  if (!(visible && accessible)) {                         \
    raise_error("Invalid static property access: %s::%s", \
                clsref->m_data.pcls->name()->data(),      \
                name->data());                            \
  }                                                       \
  if (box) {                                              \
    if (val->m_type != KindOfVariant) {                   \
      tvBox(val);                                         \
    }                                                     \
    tvDupVar(val, output);                                \
  } else {                                                \
    tvReadCell(val, output);                              \
  }                                                       \
  m_stack.popC();                                         \
  SPROP_OP_POSTLUDE                                       \
} while (0)

inline void OPTBLD_INLINE VMExecutionContext::iopCGetS(PC& pc) {
  GETS(false);
}

inline void OPTBLD_INLINE VMExecutionContext::iopCGetM(PC& pc) {
  NEXT();
  DECLARE_GETHELPER_ARGS
  getHelper(GETHELPER_ARGS);
  if (tvRet->m_type == KindOfVariant) {
    tvUnbox(tvRet);
  }
}

#define VGETH_BODY()                                                          \
  if (fr->m_type != KindOfVariant) {                                          \
    tvBox(fr);                                                                \
  }                                                                           \
  tvDup(fr, to);

inline void OPTBLD_INLINE VMExecutionContext::iopVGetL(PC& pc) {
  NEXT();
  DECODE_HA(local);
  Var* to = m_stack.allocV();
  TypedValue* fr = frame_local(m_fp, local);
  VGETH_BODY();
}

inline void OPTBLD_INLINE VMExecutionContext::iopVGetN(PC& pc) {
  NEXT();
  StringData* name;
  TypedValue* to = m_stack.topTV();
  TypedValue* fr = NULL;
  LOOKUPD_VAR(name, to, fr);
  ASSERT(fr != NULL);
  tvRefcountedDecRefCell(to);
  VGETH_BODY()
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopVGetG(PC& pc) {
  NEXT();
  StringData* name;
  TypedValue* to = m_stack.topTV();
  TypedValue* fr = NULL;
  LOOKUPD_GBL(name, to, fr);
  ASSERT(fr != NULL);
  tvRefcountedDecRefCell(to);
  VGETH_BODY()
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopVGetS(PC& pc) {
  GETS(true);
}
#undef GETS

inline void OPTBLD_INLINE VMExecutionContext::iopVGetM(PC& pc) {
  NEXT();
  DECLARE_SETHELPER_ARGS
  TypedValue* tv1 = m_stack.allocTV();
  tvWriteUninit(tv1);
  if (!setHelperPre<false, true, false, 1, ConsumeAll>(SETHELPERPRE_ARGS)) {
    if (base->m_type != KindOfVariant) {
      tvBox(base);
    }
    tvDupVar(base, tv1);
  } else {
    tvWriteNull(tv1);
    tvBox(tv1);
  }
  setHelperPost<1>(SETHELPERPOST_ARGS);
}

inline void OPTBLD_INLINE VMExecutionContext::iopIssetN(PC& pc) {
  NEXT();
  StringData* name;
  TypedValue* tv1 = m_stack.topTV();
  TypedValue* tv = NULL;
  bool e;
  LOOKUP_VAR(name, tv1, tv);
  if (tv == NULL) {
    e = false;
  } else {
    e = isset(tvAsCVarRef(tv));
  }
  tvRefcountedDecRefCell(tv1);
  tv1->m_data.num = e;
  tv1->_count = 0;
  tv1->m_type = KindOfBoolean;
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopIssetG(PC& pc) {
  NEXT();
  StringData* name;
  TypedValue* tv1 = m_stack.topTV();
  TypedValue* tv = NULL;
  bool e;
  LOOKUP_GBL(name, tv1, tv);
  if (tv == NULL) {
    e = false;
  } else {
    e = isset(tvAsCVarRef(tv));
  }
  tvRefcountedDecRefCell(tv1);
  tv1->m_data.num = e;
  tv1->_count = 0;
  tv1->m_type = KindOfBoolean;
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopIssetS(PC& pc) {
  SPROP_OP_PRELUDE
  bool e;
  if (!(visible && accessible)) {
    e = false;
  } else {
    e = isset(tvAsCVarRef(val));
  }
  m_stack.popC();
  output->m_data.num = e;
  output->_count = 0;
  output->m_type = KindOfBoolean;
  SPROP_OP_POSTLUDE
}

inline void OPTBLD_INLINE VMExecutionContext::iopIssetM(PC& pc) {
  NEXT();
  DECLARE_GETHELPER_ARGS
  getHelperPre<false, false, LeaveLast>(GETHELPERPRE_ARGS);
  // Process last member specially, in order to employ the IssetElem/IssetProp
  // operations.  (TODO combine with EmptyM.)
  bool issetResult = false;
  switch (mcode) {
  case MEL:
  case MEC: {
    issetResult = IssetEmptyElem<false>(tvScratch, tvRef, base, baseStrOff,
                                        curMember);
    break;
  }
  case MPL:
  case MPC: {
    Class* ctx = arGetContextClass(m_fp);
    issetResult = IssetEmptyProp<false>(ctx, base, curMember);
    break;
  }
  default: ASSERT(false);
  }
  getHelperPost<false>(GETHELPERPOST_ARGS);
  tvRet->m_data.num = issetResult;
  tvRet->_count = 0;
  tvRet->m_type = KindOfBoolean;
}

void VMExecutionContext::iopIssetL(PC& pc) {
  NEXT();
  DECODE_HA(local);
  TypedValue* tv = frame_local(m_fp, local);
  bool ret = isset(tvAsCVarRef(tv));
  TypedValue* topTv = m_stack.allocTV();
  topTv->m_data.num = ret;
  topTv->m_type = KindOfBoolean;
}

#define IOP_TYPE_CHECK_INSTR(what, predicate)                       \
void OPTBLD_INLINE VMExecutionContext::iopIs ## what ## L(PC& pc) { \
  NEXT();                                                           \
  DECODE_HA(local);                                                 \
  TypedValue* tv = frame_local(m_fp, local);                        \
  if (tv->m_type == KindOfUninit) {                                 \
    RAISE_UNDEFINED_LOCAL(tv);                                      \
  }                                                                 \
  bool ret = predicate(tvAsCVarRef(tv));                            \
  TypedValue* topTv = m_stack.allocTV();                            \
  topTv->m_data.num = ret;                                          \
  topTv->m_type = KindOfBoolean;                                    \
}                                                                   \
                                                                    \
void OPTBLD_INLINE VMExecutionContext::iopIs ## what ## C(PC& pc) { \
  NEXT();                                                           \
  TypedValue* topTv = m_stack.topTV();                              \
  ASSERT(topTv->m_type != KindOfVariant);                           \
  bool ret = predicate(tvAsCVarRef(topTv));                         \
  tvRefcountedDecRefCell(topTv);                                    \
  topTv->m_data.num = ret;                                          \
  topTv->m_type = KindOfBoolean;                                    \
}

IOP_TYPE_CHECK_INSTR(Null,   f_is_null)
IOP_TYPE_CHECK_INSTR(Bool,   f_is_bool)
IOP_TYPE_CHECK_INSTR(Int,    f_is_int)
IOP_TYPE_CHECK_INSTR(Double, f_is_double)
IOP_TYPE_CHECK_INSTR(String, f_is_string)
IOP_TYPE_CHECK_INSTR(Array,  f_is_array)
IOP_TYPE_CHECK_INSTR(Object, f_is_object)
#undef IOP_TYPE_CHECK_INSTR

inline void OPTBLD_INLINE VMExecutionContext::iopEmptyL(PC& pc) {
  NEXT();
  DECODE_HA(local);
  TypedValue* loc = frame_local(m_fp, local);
  bool e = empty(tvAsCVarRef(loc));
  TypedValue* tv1 = m_stack.allocTV();
  tv1->m_data.num = e;
  tv1->_count = 0;
  tv1->m_type = KindOfBoolean;
}

inline void OPTBLD_INLINE VMExecutionContext::iopEmptyN(PC& pc) {
  NEXT();
  StringData* name;
  TypedValue* tv1 = m_stack.topTV();
  TypedValue* tv = NULL;
  bool e;
  LOOKUP_VAR(name, tv1, tv);
  if (tv == NULL) {
    e = true;
  } else {
    e = empty(tvAsCVarRef(tv));
  }
  tvRefcountedDecRefCell(tv1);
  tv1->m_data.num = e;
  tv1->_count = 0;
  tv1->m_type = KindOfBoolean;
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopEmptyG(PC& pc) {
  NEXT();
  StringData* name;
  TypedValue* tv1 = m_stack.topTV();
  TypedValue* tv = NULL;
  bool e;
  LOOKUP_GBL(name, tv1, tv);
  if (tv == NULL) {
    e = true;
  } else {
    e = empty(tvAsCVarRef(tv));
  }
  tvRefcountedDecRefCell(tv1);
  tv1->m_data.num = e;
  tv1->_count = 0;
  tv1->m_type = KindOfBoolean;
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopEmptyS(PC& pc) {
  SPROP_OP_PRELUDE
  bool e;
  if (!(visible && accessible)) {
    e = true;
  } else {
    e = empty(tvAsCVarRef(val));
  }
  m_stack.popC();
  output->m_data.num = e;
  output->_count = 0;
  output->m_type = KindOfBoolean;
  SPROP_OP_POSTLUDE
}

inline void OPTBLD_INLINE VMExecutionContext::iopEmptyM(PC& pc) {
  NEXT();
  DECLARE_GETHELPER_ARGS
  getHelperPre<false, false, LeaveLast>(GETHELPERPRE_ARGS);
  // Process last member specially, in order to employ the EmptyElem/EmptyProp
  // operations.  (TODO combine with IssetM)
  bool emptyResult = false;
  switch (mcode) {
  case MEL:
  case MEC: {
    emptyResult = IssetEmptyElem<true>(tvScratch, tvRef, base, baseStrOff,
                                       curMember);
    break;
  }
  case MPL:
  case MPC: {
    Class* ctx = arGetContextClass(m_fp);
    emptyResult = IssetEmptyProp<true>(ctx, base, curMember);
    break;
  }
  default: ASSERT(false);
  }
  getHelperPost<false>(GETHELPERPOST_ARGS);
  tvRet->m_data.num = emptyResult;
  tvRet->_count = 0;
  tvRet->m_type = KindOfBoolean;
}

inline void OPTBLD_INLINE VMExecutionContext::iopSetL(PC& pc) {
  NEXT();
  DECODE_HA(local);
  ASSERT(local < m_fp->m_func->numLocals());
  Cell* fr = m_stack.topC();
  TypedValue* to = frame_local(m_fp, local);
  tvSet(fr, to);
}

inline void OPTBLD_INLINE VMExecutionContext::iopSetN(PC& pc) {
  NEXT();
  StringData* name;
  Cell* fr = m_stack.topC();
  TypedValue* tv2 = m_stack.indTV(1);
  TypedValue* to = NULL;
  LOOKUPD_VAR(name, tv2, to);
  ASSERT(to != NULL);
  tvSet(fr, to);
  memcpy((void*)tv2, (void*)fr, sizeof(TypedValue));
  m_stack.discard();
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopSetG(PC& pc) {
  NEXT();
  StringData* name;
  Cell* fr = m_stack.topC();
  TypedValue* tv2 = m_stack.indTV(1);
  TypedValue* to = NULL;
  LOOKUPD_GBL(name, tv2, to);
  ASSERT(to != NULL);
  tvSet(fr, to);
  memcpy((void*)tv2, (void*)fr, sizeof(TypedValue));
  m_stack.discard();
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopSetS(PC& pc) {
  NEXT();
  TypedValue* tv1 = m_stack.topTV();
  TypedValue* classref = m_stack.indTV(1);
  TypedValue* propn = m_stack.indTV(2);
  TypedValue* output = propn;
  StringData* name;
  TypedValue* val;
  bool visible, accessible;
  LOOKUP_SPROP(classref, name, propn, val, visible, accessible);
  if (!(visible && accessible)) {
    raise_error("Invalid static property access: %s::%s",
                classref->m_data.pcls->name()->data(),
                name->data());
  }
  tvSet(tv1, val);
  tvRefcountedDecRefCell(propn);
  memcpy(output, tv1, sizeof(TypedValue));
  m_stack.ndiscard(2);
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopSetM(PC& pc) {
  NEXT();
  DECLARE_SETHELPER_ARGS
  if (!setHelperPre<false, true, false, 1, LeaveLast>(SETHELPERPRE_ARGS)) {
    Cell* c1 = m_stack.topC();

    if (mcode == MW) {
      SetNewElem(base, c1);
    } else {
      switch (mcode) {
      case MEL:
      case MEC:
        SetElem(base, curMember, c1);
        break;
      case MPL:
      case MPC: {
        Class* ctx = arGetContextClass(m_fp);
        SetProp(ctx, base, curMember, c1);
        break;
      }
      default: ASSERT(false);
      }
    }
  }
  setHelperPost<1>(SETHELPERPOST_ARGS);
}

inline void OPTBLD_INLINE VMExecutionContext::iopSetOpL(PC& pc) {
  NEXT();
  DECODE_HA(local);
  DECODE(unsigned char, op);
  Cell* fr = m_stack.topC();
  TypedValue* to = frame_local(m_fp, local);
  SETOP_BODY(to, op, fr);
  tvRefcountedDecRefCell(fr);
  tvReadCell(to, fr);
}

inline void OPTBLD_INLINE VMExecutionContext::iopSetOpN(PC& pc) {
  NEXT();
  DECODE(unsigned char, op);
  StringData* name;
  Cell* fr = m_stack.topC();
  TypedValue* tv2 = m_stack.indTV(1);
  TypedValue* to = NULL;
  // XXX We're probably not getting warnings totally correct here
  LOOKUPD_VAR(name, tv2, to);
  ASSERT(to != NULL);
  SETOP_BODY(to, op, fr);
  tvRefcountedDecRef(fr);
  tvRefcountedDecRef(tv2);
  tvReadCell(to, tv2);  
  m_stack.discard();
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopSetOpG(PC& pc) {
  NEXT();
  DECODE(unsigned char, op);
  StringData* name;
  Cell* fr = m_stack.topC();
  TypedValue* tv2 = m_stack.indTV(1);
  TypedValue* to = NULL;
  // XXX We're probably not getting warnings totally correct here
  LOOKUPD_GBL(name, tv2, to);
  ASSERT(to != NULL);
  SETOP_BODY(to, op, fr);
  tvRefcountedDecRef(fr);
  tvRefcountedDecRef(tv2);
  tvReadCell(to, tv2);
  m_stack.discard();
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopSetOpS(PC& pc) {
  NEXT();
  DECODE(unsigned char, op);
  Cell* fr = m_stack.topC();
  TypedValue* classref = m_stack.indTV(1);
  TypedValue* propn = m_stack.indTV(2);
  TypedValue* output = propn;
  StringData* name;
  TypedValue* val;
  bool visible, accessible;
  LOOKUP_SPROP(classref, name, propn, val, visible, accessible);
  if (!(visible && accessible)) {
    raise_error("Invalid static property access: %s::%s",
                classref->m_data.pcls->name()->data(),
                name->data());
  }
  SETOP_BODY(val, op, fr);
  tvRefcountedDecRefCell(propn);
  tvRefcountedDecRef(fr);
  tvReadCell(val, output);
  m_stack.ndiscard(2);
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopSetOpM(PC& pc) {
  NEXT();
  DECODE(unsigned char, op);
  DECLARE_SETHELPER_ARGS
  if (!setHelperPre<MoreWarnings, true, false, 1,
      LeaveLast>(SETHELPERPRE_ARGS)) {
    TypedValue* result;
    Cell* rhs = m_stack.topC();

    if (mcode == MW) {
      result = SetOpNewElem(tvScratch, tvRef, op, base, rhs);
    } else {
      switch (mcode) {
      case MEL:
      case MEC:
        result = SetOpElem(tvScratch, tvRef, op, base, curMember, rhs);
        break;
      case MPL:
      case MPC: {
        Class *ctx = arGetContextClass(m_fp);
        result = SetOpProp(tvScratch, tvRef, ctx, op, base, curMember, rhs);
        break;
      }
      default:
        ASSERT(false);
        result = NULL; // Silence compiler warning.
      }
    }

    if (result->m_type == KindOfVariant) {
      tvUnbox(result);
    }
    tvRefcountedDecRef(rhs);
    tvDup(result, rhs);
  }
  setHelperPost<1>(SETHELPERPOST_ARGS);
}

inline void OPTBLD_INLINE VMExecutionContext::iopIncDecL(PC& pc) {
  NEXT();
  DECODE_HA(local);
  DECODE(unsigned char, op);
  TypedValue* to = m_stack.allocTV();
  TypedValue* fr = frame_local(m_fp, local);
  IncDecBody(op, fr, to);
}

inline void OPTBLD_INLINE VMExecutionContext::iopIncDecN(PC& pc) {
  NEXT();
  DECODE(unsigned char, op);
  StringData* name;
  TypedValue* nameCell = m_stack.topTV();
  TypedValue* local = NULL;
  // XXX We're probably not getting warnings totally correct here
  LOOKUPD_VAR(name, nameCell, local);
  ASSERT(local != NULL);
  IncDecBody(op, local, nameCell);
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopIncDecG(PC& pc) {
  NEXT();
  DECODE(unsigned char, op);
  StringData* name;
  TypedValue* nameCell = m_stack.topTV();
  TypedValue* gbl = NULL;
  // XXX We're probably not getting warnings totally correct here
  LOOKUPD_GBL(name, nameCell, gbl);
  ASSERT(gbl != NULL);
  IncDecBody(op, gbl, nameCell);
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopIncDecS(PC& pc) {
  SPROP_OP_PRELUDE
  DECODE(unsigned char, op);
  if (!(visible && accessible)) {
    raise_error("Invalid static property access: %s::%s",
                clsref->m_data.pcls->name()->data(),
                name->data());
  }
  tvRefcountedDecRefCell(nameCell);
  IncDecBody(op, val, output);
  m_stack.discard();
  SPROP_OP_POSTLUDE
}

inline void OPTBLD_INLINE VMExecutionContext::iopIncDecM(PC& pc) {
  NEXT();
  DECODE(unsigned char, op);
  DECLARE_SETHELPER_ARGS
  TypedValue to;
  tvWriteUninit(&to);
  if (!setHelperPre<MoreWarnings, true, false, 0,
      LeaveLast>(SETHELPERPRE_ARGS)) {
    if (mcode == MW) {
      IncDecNewElem(tvScratch, tvRef, op, base, to);
    } else {
      switch (mcode) {
      case MEL:
      case MEC:
        IncDecElem(tvScratch, tvRef, op, base, curMember, to);
        break;
      case MPL:
      case MPC: {
        Class* ctx = arGetContextClass(m_fp);
        IncDecProp(tvScratch, tvRef, ctx, op, base, curMember, to);
        break;
      }
      default: ASSERT(false);
      }
    }
  }
  setHelperPost<0>(SETHELPERPOST_ARGS);
  Cell* c1 = m_stack.allocC();
  memcpy(c1, &to, sizeof(TypedValue));
}

inline void OPTBLD_INLINE VMExecutionContext::iopBindL(PC& pc) {
  NEXT();
  DECODE_HA(local);
  Var* fr = m_stack.topV();
  TypedValue* to = frame_local(m_fp, local);
  tvBind(fr, to);
}

inline void OPTBLD_INLINE VMExecutionContext::iopBindN(PC& pc) {
  NEXT();
  StringData* name;
  TypedValue* fr = m_stack.topTV();
  TypedValue* nameTV = m_stack.indTV(1);
  TypedValue* to = NULL;
  LOOKUPD_VAR(name, nameTV, to);
  ASSERT(to != NULL);
  tvBind(fr, to);
  memcpy((void*)nameTV, (void*)fr, sizeof(TypedValue));
  m_stack.discard();
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopBindG(PC& pc) {
  NEXT();
  StringData* name;
  TypedValue* fr = m_stack.topTV();
  TypedValue* nameTV = m_stack.indTV(1);
  TypedValue* to = NULL;
  LOOKUPD_GBL(name, nameTV, to);
  ASSERT(to != NULL);
  tvBind(fr, to);
  memcpy((void*)nameTV, (void*)fr, sizeof(TypedValue));
  m_stack.discard();
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopBindS(PC& pc) {
  NEXT();
  TypedValue* fr = m_stack.topTV();
  TypedValue* classref = m_stack.indTV(1);
  TypedValue* propn = m_stack.indTV(2);
  TypedValue* output = propn;
  StringData* name;
  TypedValue* val;
  bool visible, accessible;
  LOOKUP_SPROP(classref, name, propn, val, visible, accessible);
  if (!(visible && accessible)) {
    raise_error("Invalid static property access: %s::%s",
                classref->m_data.pcls->name()->data(),
                name->data());
  }
  tvBind(fr, val);
  tvRefcountedDecRefCell(propn);
  memcpy(output, fr, sizeof(TypedValue));
  m_stack.ndiscard(2);
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopBindM(PC& pc) {
  NEXT();
  DECLARE_SETHELPER_ARGS
  TypedValue* tv1 = m_stack.topTV();
  if (!setHelperPre<false, true, false, 1, ConsumeAll>(SETHELPERPRE_ARGS)) {
    // Bind the element/property with the var on the top of the stack
    tvBind(tv1, base);
  }
  setHelperPost<1>(SETHELPERPOST_ARGS);
}

inline void OPTBLD_INLINE VMExecutionContext::iopUnsetL(PC& pc) {
  NEXT();
  DECODE_HA(local);
  ASSERT(local < m_fp->m_func->numLocals());
  TypedValue* tv = frame_local(m_fp, local);
  tvRefcountedDecRef(tv);
  TV_WRITE_UNINIT(tv);
}

inline void OPTBLD_INLINE VMExecutionContext::iopUnsetN(PC& pc) {
  NEXT();
  StringData* name;
  TypedValue* tv1 = m_stack.topTV();
  TypedValue* tv = NULL;
  LOOKUP_VAR(name, tv1, tv);
  ASSERT(!m_fp->hasInvName());
  if (tv != NULL) {
    tvRefcountedDecRef(tv);
    TV_WRITE_UNINIT(tv);
  } else if (m_fp->m_varEnv != NULL) {
    m_fp->m_varEnv->unset(name);
  }
  m_stack.popC();
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopUnsetG(PC& pc) {
  NEXT();
  StringData* name;
  TypedValue* tv1 = m_stack.topTV();
  LOOKUP_NAME(name, tv1);
  VarEnv* varEnv = g_vmContext->m_varEnvs.front();
  ASSERT(varEnv != NULL);
  varEnv->unset(name);
  m_stack.popC();
  LITSTR_DECREF(name);
}

inline void OPTBLD_INLINE VMExecutionContext::iopUnsetM(PC& pc) {
  NEXT();
  DECLARE_SETHELPER_ARGS
  if (!setHelperPre<false, false, true, 0, LeaveLast>(SETHELPERPRE_ARGS)) {
    switch (mcode) {
    case MEL:
    case MEC:
      UnsetElem(base, curMember);
      break;
    case MPL:
    case MPC: {
      Class* ctx = arGetContextClass(m_fp);
      UnsetProp(ctx, base, curMember);
      break;
    }
    default: ASSERT(false);
    }
  }
  setHelperPost<0>(SETHELPERPOST_ARGS);
}

inline void OPTBLD_INLINE VMExecutionContext::iopFPushFunc(PC& pc) {
  NEXT();
  DECODE_IVA(numArgs);
  Cell* c1 = m_stack.topC();
  const Func* func = NULL;
  ObjectData* origObj = NULL;
  StringData* origSd = NULL;
  if (IS_STRING_TYPE(c1->m_type)) {
    origSd = c1->m_data.pstr;
    func = Unit::lookupFunc(origSd);
  } else if (c1->m_type == KindOfObject) {
    static StringData* invokeName = StringData::GetStaticString("__invoke");
    origObj = c1->m_data.pobj;
    const Class* cls = origObj->getVMClass();
    func = cls->lookupMethod(invokeName);
    if (func == NULL) {
      raise_error("Function name must be a string");
    }
  } else {
    raise_error("Function name must be a string");
  }
  if (func == NULL) {
    raise_error("Undefined function: %s", c1->m_data.pstr->data());
  }
  ASSERT(!origObj || !origSd);
  ASSERT(origObj || origSd);
  // We've already saved origObj or origSd; we'll use them after
  // overwriting the pointer on the stack.  Don't refcount it now; defer
  // till after we're done with it.
  m_stack.discard();
  ActRec* ar = m_stack.allocA();
  ar->m_func = func;
  arSetSfp(ar, m_fp);
  if (origObj) {
    if (func->attrs() & AttrStatic) {
      ar->setClass(origObj->getVMClass());
      if (origObj->decRefCount() == 0) {
        origObj->release();
      }
    } else {
      ar->setThis(origObj);
      // Teleport the reference from the destroyed stack cell to the
      // ActRec. Don't try this at home.
    }
  } else {
    ar->setThis(NULL);
    if (origSd->decRefCount() == 0) {
      origSd->release();
    }
  }
  ar->initNumArgs(numArgs);
  ar->setVarEnv(NULL);
}

inline void OPTBLD_INLINE VMExecutionContext::iopFPushFuncD(PC& pc) {
  NEXT();
  DECODE_IVA(numArgs);
  DECODE(Id, id);
  const NamedEntityPair nep = m_fp->m_func->unit()->lookupNamedEntityPairId(id);
  Func* func = Unit::lookupFunc(nep.second, nep.first);
  if (func == NULL) {
    raise_error("Undefined function: %s",
                m_fp->m_func->unit()->lookupLitstrId(id)->data());
  }
  DEBUGGER_IF(phpBreakpointEnabled(func->name()->data()));
  ActRec* ar = m_stack.allocA();
  arSetSfp(ar, m_fp);
  ar->m_func = func;
  ar->setThis(NULL);
  ar->initNumArgs(numArgs);
  ar->setVarEnv(NULL);
}

#define OBJMETHOD_BODY(cls, name, obj) do { \
  const Func* f; \
  LookupResult res = lookupObjMethod(f, cls, name, true); \
  ASSERT(f); \
  ActRec* ar = m_stack.allocA(); \
  arSetSfp(ar, m_fp); \
  ar->m_func = f; \
  if (res == MethodFoundNoThis) { \
    if (obj->decRefCount() == 0) obj->release(); \
    ar->setClass(cls); \
  } else { \
    ASSERT(res == MethodFoundWithThis || res == MagicCallFound); \
    /* Transfer ownership of obj to the ActRec*/ \
    ar->setThis(obj); \
  } \
  ar->initNumArgs(numArgs); \
  if (res == MagicCallFound) { \
    ar->setInvName(name); \
  } else { \
    ar->setVarEnv(NULL); \
    LITSTR_DECREF(name); \
  } \
} while (0)

inline void OPTBLD_INLINE VMExecutionContext::iopFPushObjMethod(PC& pc) {
  NEXT();
  DECODE_IVA(numArgs);
  Cell* c1 = m_stack.topC(); // Method name.
  if (!IS_STRING_TYPE(c1->m_type)) {
    raise_error("FPushObjMethod method argument must be a string");
  }
  Cell* c2 = m_stack.indC(1); // Object.
  if (c2->m_type != KindOfObject) {
    throw_call_non_object(c1->m_data.pstr->data());
  }
  ObjectData* obj = c2->m_data.pobj;
  Class* cls = obj->getVMClass();
  StringData* name = c1->m_data.pstr;
  // We handle decReffing obj and name below
  m_stack.ndiscard(2);
  OBJMETHOD_BODY(cls, name, obj);
}

inline void OPTBLD_INLINE VMExecutionContext::iopFPushObjMethodD(PC& pc) {
  NEXT();
  DECODE_IVA(numArgs);
  DECODE_LITSTR(name);
  Cell* c1 = m_stack.topC();
  if (c1->m_type != KindOfObject) {
    throw_call_non_object(name->data());
  }
  ObjectData* obj = c1->m_data.pobj;
  Class* cls = obj->getVMClass();
  // We handle decReffing obj below
  m_stack.discard();
  OBJMETHOD_BODY(cls, name, obj);
}

#define CLSMETHOD_BODY(cls, name, obj, forwarding) do { \
  const Func* f; \
  LookupResult res = lookupClsMethod(f, cls, name, obj, true); \
  if (f->isAbstract()) { \
    raise_error("Cannot call abstract method %s()", \
                f->fullName()->data()); \
  } \
  if (res == MethodFoundNoThis || res == MagicCallStaticFound) { \
    obj = NULL; \
  } else { \
    ASSERT(obj); \
    ASSERT(res == MethodFoundWithThis || res == MagicCallFound); \
    obj->incRefCount(); \
  } \
  ASSERT(f); \
  ActRec* ar = m_stack.allocA(); \
  arSetSfp(ar, m_fp); \
  ar->m_func = f; \
  if (obj) { \
    ar->setThis(obj); \
  } else { \
    if (!forwarding) { \
      ar->setClass(cls); \
    } else { \
      /* Propogate the current late bound class if there is one, */ \
      /* otherwise use the class given by this instruction's input */ \
      if (m_fp->hasThis()) { \
        cls = m_fp->getThis()->getVMClass(); \
      } else if (m_fp->hasClass()) { \
        cls = m_fp->getClass(); \
      } \
      ar->setClass(cls); \
    } \
  } \
  ar->initNumArgs(numArgs); \
  if (res == MagicCallFound || res == MagicCallStaticFound) { \
    ar->setInvName(name); \
  } else { \
    ar->setVarEnv(NULL); \
    LITSTR_DECREF(const_cast<StringData*>(name)); \
  } \
} while (0)

inline void OPTBLD_INLINE VMExecutionContext::iopFPushClsMethod(PC& pc) {
  NEXT();
  DECODE_IVA(numArgs);
  Cell* c1 = m_stack.indC(1); // Method name.
  if (!IS_STRING_TYPE(c1->m_type)) {
    raise_error("FPushClsMethod method argument must be a string");
  }
  TypedValue* tv = m_stack.top();
  ASSERT(tv->m_type == KindOfClass);
  Class* cls = tv->m_data.pcls;
  StringData* name = c1->m_data.pstr;
  // CLSMETHOD_BODY will take care of decReffing name
  m_stack.ndiscard(2);
  ASSERT(cls && name);
  ObjectData* obj = m_fp->hasThis() ? m_fp->getThis() : NULL;
  CLSMETHOD_BODY(cls, name, obj, false);
}

inline void OPTBLD_INLINE VMExecutionContext::iopFPushClsMethodD(PC& pc) {
  NEXT();
  DECODE_IVA(numArgs);
  DECODE_LITSTR(name);
  DECODE(Id, classId);
  const NamedEntityPair &nep =
    m_fp->m_func->unit()->lookupNamedEntityPairId(classId);
  Class* cls = Unit::loadClass(nep.second, nep.first);
  if (cls == NULL) {
    raise_error(Strings::UNKNOWN_CLASS, nep.first->data());
  }
  ObjectData* obj = m_fp->hasThis() ? m_fp->getThis() : NULL;
  CLSMETHOD_BODY(cls, name, obj, false);
}

inline void OPTBLD_INLINE VMExecutionContext::iopFPushClsMethodF(PC& pc) {
  NEXT();
  DECODE_IVA(numArgs);
  Cell* c1 = m_stack.indC(1); // Method name.
  if (!IS_STRING_TYPE(c1->m_type)) {
    raise_error("FPushClsMethodF method argument must be a string");
  }
  TypedValue* tv = m_stack.top();
  ASSERT(tv->m_type == KindOfClass);
  Class* cls = tv->m_data.pcls;
  ASSERT(cls);
  StringData* name = c1->m_data.pstr;
  // CLSMETHOD_BODY will take care of decReffing name
  m_stack.ndiscard(2);
  ObjectData* obj = m_fp->hasThis() ? m_fp->getThis() : NULL;
  CLSMETHOD_BODY(cls, name, obj, true);
}

#undef CLSMETHOD_BODY

inline void OPTBLD_INLINE VMExecutionContext::iopFPushCtor(PC& pc) {
  NEXT();
  DECODE_IVA(numArgs);
  TypedValue* tv = m_stack.topTV();
  ASSERT(tv->m_type == KindOfClass);
  Class* cls = tv->m_data.pcls;
  ASSERT(cls != NULL);
  // Lookup the ctor
  const Func* f;
  LookupResult res UNUSED = lookupCtorMethod(f, cls, true);
  ASSERT(res == MethodFoundWithThis);
  // Replace input with uninitialized instance.
  ObjectData* this_ = newInstance(cls);
  TRACE(2, "FPushCtor: just new'ed an instance of class %s: %p\n",
        cls->name()->data(), this_);
  this_->incRefCount();
  this_->incRefCount();
  tv->m_type = KindOfObject;
  tv->_count = 0;
  tv->m_data.pobj = this_;
  // Push new activation record.
  ActRec* ar = m_stack.allocA();
  arSetSfp(ar, m_fp);
  ar->m_func = f;
  ar->setThis(this_);
  ar->initNumArgs(numArgs, true /* isFPushCtor */);
  arSetSfp(ar, m_fp);
  ar->setVarEnv(NULL);
}

inline void OPTBLD_INLINE VMExecutionContext::iopFPushCtorD(PC& pc) {
  NEXT();
  DECODE_IVA(numArgs);
  DECODE(Id, id);
  const NamedEntityPair &nep =
    m_fp->m_func->unit()->lookupNamedEntityPairId(id);
  Class* cls = Unit::loadClass(nep.second, nep.first);
  if (cls == NULL) {
    raise_error("Undefined class: %s",
                m_fp->m_func->unit()->lookupLitstrId(id)->data());
  }
  // Lookup the ctor
  const Func* f;
  LookupResult res UNUSED = lookupCtorMethod(f, cls, true);
  ASSERT(res == MethodFoundWithThis);
  // Push uninitialized instance.
  ObjectData* this_ = newInstance(cls);
  TRACE(2, "FPushCtorD: new'ed an instance of class %s: %p\n",
        cls->name()->data(), this_);
  this_->incRefCount();
  m_stack.pushObject(this_);
  // Push new activation record.
  ActRec* ar = m_stack.allocA();
  arSetSfp(ar, m_fp);
  ar->m_func = f;
  ar->setThis(this_);
  ar->initNumArgs(numArgs, true /* isFPushCtor */);
  ar->setVarEnv(NULL);
}

static inline ActRec* arFromInstr(TypedValue* sp, const Opcode* pc) {
  return arFromSpOffset((ActRec*)sp, instrSpToArDelta(pc));
}

inline void OPTBLD_INLINE VMExecutionContext::iopFPassC(PC& pc) {
#ifdef DEBUG
  ActRec* ar = arFromInstr(m_stack.top(), (Opcode*)pc);
#endif
  NEXT();
  DECODE_IVA(paramId);
#ifdef DEBUG
  ASSERT(paramId < ar->numArgs());
#endif
}

#define FPASSC_CHECKED_PRELUDE                                                \
  ActRec* ar = arFromInstr(m_stack.top(), (Opcode*)pc);                       \
  NEXT();                                                                     \
  DECODE_IVA(paramId);                                                        \
  ASSERT(paramId < ar->numArgs());                                            \
  const Func* func = ar->m_func;

inline void OPTBLD_INLINE VMExecutionContext::iopFPassCW(PC& pc) {
  FPASSC_CHECKED_PRELUDE
  if (func->mustBeRef(paramId)) {
    TRACE(1, "FPassCW: function %s(%d) param %d is by reference, "
          "raising a strict warning (attr:0x%x)\n",
          func->name()->data(), func->numParams(), paramId,
          func->isBuiltin() ? func->info()->attribute : 0);
    raise_strict_warning("Only variables should be passed by reference");
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopFPassCE(PC& pc) {
  FPASSC_CHECKED_PRELUDE
  if (func->mustBeRef(paramId)) {
    TRACE(1, "FPassCE: function %s(%d) param %d is by reference, "
          "throwing a fatal error (attr:0x%x)\n",
          func->name()->data(), func->numParams(), paramId,
          func->isBuiltin() ? func->info()->attribute : 0);
    raise_error("Cannot pass parameter %d by reference", paramId+1);
  }
}

#undef FPASSC_CHECKED_PRELUDE

inline void OPTBLD_INLINE VMExecutionContext::iopFPassV(PC& pc) {
  ActRec* ar = arFromInstr(m_stack.top(), (Opcode*)pc);
  NEXT();
  DECODE_IVA(paramId);
  ASSERT(paramId < ar->numArgs());
  const Func* func = ar->m_func;
  if (!func->byRef(paramId)) {
    m_stack.unbox();
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopFPassR(PC& pc) {
  ActRec* ar = arFromInstr(m_stack.top(), (Opcode*)pc);
  NEXT();
  DECODE_IVA(paramId);
  ASSERT(paramId < ar->numArgs());
  const Func* func = ar->m_func;
  if (func->byRef(paramId)) {
    TypedValue* tv = m_stack.topTV();
    if (tv->m_type != KindOfVariant) {
      tvBox(tv);
    }
  } else {
    if (m_stack.topTV()->m_type == KindOfVariant) {
      m_stack.unbox();
    }
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopFPassL(PC& pc) {
  ActRec* ar = arFromInstr(m_stack.top(), (Opcode*)pc);
  NEXT();
  DECODE_IVA(paramId);
  DECODE_HA(local);
  ASSERT(paramId < ar->numArgs());
  TypedValue* fr = frame_local(m_fp, local);
  TypedValue* to = m_stack.allocTV();
  if (!ar->m_func->byRef(paramId)) {
    CGETH_BODY();
  } else {
    VGETH_BODY();
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopFPassN(PC& pc) {
  ActRec* ar = arFromInstr(m_stack.top(), (Opcode*)pc);
  PC origPc = pc;
  NEXT();
  DECODE_IVA(paramId);
  ASSERT(paramId < ar->numArgs());
  if (!ar->m_func->byRef(paramId)) {
    iopCGetN(origPc);
  } else {
    iopVGetN(origPc);
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopFPassG(PC& pc) {
  ActRec* ar = arFromInstr(m_stack.top(), (Opcode*)pc);
  PC origPc = pc;
  NEXT();
  DECODE_IVA(paramId);
  ASSERT(paramId < ar->numArgs());
  if (!ar->m_func->byRef(paramId)) {
    iopCGetG(origPc);
  } else {
    iopVGetG(origPc);
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopFPassS(PC& pc) {
  ActRec* ar = arFromInstr(m_stack.top(), (Opcode*)pc);
  PC origPc = pc;
  NEXT();
  DECODE_IVA(paramId);
  ASSERT(paramId < ar->numArgs());
  if (!ar->m_func->byRef(paramId)) {
    iopCGetS(origPc);
  } else {
    iopVGetS(origPc);
  }
}

void VMExecutionContext::iopFPassM(PC& pc) {
  ActRec* ar = arFromInstr(m_stack.top(), (Opcode*)pc);
  NEXT();
  DECODE_IVA(paramId);
  ASSERT(paramId < ar->numArgs());
  if (!ar->m_func->byRef(paramId)) {
    DECLARE_GETHELPER_ARGS
    getHelper(GETHELPER_ARGS);
    if (tvRet->m_type == KindOfVariant) {
      tvUnbox(tvRet);
    }
  } else {
    DECLARE_SETHELPER_ARGS
    TypedValue* tv1 = m_stack.allocTV();
    tvWriteUninit(tv1);
    if (!setHelperPre<false, true, false, 1,
        ConsumeAll>(SETHELPERPRE_ARGS)) {
      if (base->m_type != KindOfVariant) {
        tvBox(base);
      }
      tvDupVar(base, tv1);
    } else {
      tvWriteNull(tv1);
      tvBox(tv1);
    }
    setHelperPost<1>(SETHELPERPOST_ARGS);
  }
}

template <bool handle_throw>
void VMExecutionContext::doFCall(ActRec* ar, PC& pc) {
  ASSERT(ar->m_savedRbp == (uint64_t)m_fp);
  ar->m_savedRip = (uintptr_t)m_transl->getRetFromInterpretedFrame();
  TRACE(3, "FCall: pc %p func %p base %d\n", m_pc,
        m_fp->m_func->unit()->entry(),
        int(m_fp->m_func->base()));
  ar->m_soff = m_fp->m_func->unit()->offsetOf(pc)
    - (uintptr_t)m_fp->m_func->base();
  ASSERT(pcOff() > m_fp->m_func->base());
  prepareFuncEntry<false, handle_throw>(ar, pc);
  SYNC();
  EventHook::FunctionEnter(ar, EventHook::NormalFunc);
  INST_HOOK_FENTRY(ar->m_func->fullName());
}

template void VMExecutionContext::doFCall<true>(ActRec *ar, PC& pc);

inline void OPTBLD_INLINE VMExecutionContext::iopFCall(PC& pc) {
  ActRec* ar = arFromInstr(m_stack.top(), (Opcode*)pc);
  NEXT();
  DECODE_IVA(numArgs);
  ASSERT(numArgs == ar->numArgs());
  checkStack(m_stack, ar->m_func);
  doFCall<false>(ar, pc);
}

inline void OPTBLD_INLINE VMExecutionContext::iopIterInit(PC& pc) {
  PC origPc = pc;
  NEXT();
  DECODE_IA(itId);
  DECODE(Offset, offset);
  Cell* c1 = m_stack.topC();
  if (c1->m_type == KindOfArray) {
    if (!c1->m_data.parr->empty()) {
      Iter* it = FP2ITER(m_fp, itId);
      (void) new (&it->arr()) ArrayIter(c1->m_data.parr); // call CTor
      it->m_itype = Iter::TypeArray;
    } else {
      ITER_SKIP(offset);
    }
  } else if (c1->m_type == KindOfObject) {
    Class* ctx = arGetContextClass(m_fp);
    Iter* it = FP2ITER(m_fp, itId);
    CStrRef ctxStr = ctx ? ctx->nameRef() : null_string;
    bool isIterator;
    Object obj = c1->m_data.pobj->iterableObject(isIterator);
    if (isIterator) {
      (void) new (&it->arr()) ArrayIter(obj.get());
    } else {
      Array iterArray(obj->o_toIterArray(ctxStr));
      ArrayData* ad = iterArray.getArrayData();
      (void) new (&it->arr()) ArrayIter(ad);
    }
    if (it->arr().end()) {
      // Iterator was empty; call the destructor on the iterator we
      // just constructed and branch to done case
      it->arr().~ArrayIter();
      ITER_SKIP(offset);
    } else {
      it->m_itype = (isIterator ? Iter::TypeIterator : Iter::TypeArray);
    }
  } else {
    raise_warning("Invalid argument supplied for foreach()");
    ITER_SKIP(offset);
  }
  m_stack.popC();
}

inline void OPTBLD_INLINE VMExecutionContext::iopIterInitM(PC& pc) {
  PC origPc = pc;
  NEXT();
  DECODE_IA(itId);
  DECODE(Offset, offset);
  Var* v1 = m_stack.topV();
  tvAsVariant(v1).escalate(true);
  if (v1->m_data.ptv->m_type == KindOfArray) {
    ArrayData* ad = v1->m_data.ptv->m_data.parr;
    if (!ad->empty()) {
      Iter* it = FP2ITER(m_fp, itId);
      MIterCtx& mi = it->marr();
      if (ad->getCount() > 1) {
        ArrayData* copy = ad->copy();
        copy->incRefCount();
        ad->decRefCount();  // count > 1 to begin with; don't need release
        v1->m_data.ptv->m_data.parr = copy;
      }
      (void) new (&mi) MIterCtx((const Variant*)v1->m_data.ptv);
      it->m_itype = Iter::TypeMutableArray;
      mi.m_mArray->advance();
    } else {
      ITER_SKIP(offset);
    }
  } else if (v1->m_data.ptv->m_type == KindOfObject)  {
    Class* ctx = arGetContextClass(m_fp);
    CStrRef ctxStr = ctx ? ctx->nameRef() : null_string;

    bool isIterator;
    Object obj = v1->m_data.ptv->m_data.pobj->iterableObject(isIterator);
    if (isIterator) {
      raise_error("An iterator cannot be used with foreach by reference");
    }
    Array iterArray = obj->o_toIterArray(ctxStr, true);
    ArrayData* ad = iterArray.getArrayData();
    if (ad->empty()) {
      ITER_SKIP(offset);
    } else {
      if (ad->getCount() > 1) {
        ArrayData* copy = ad->copy();
        copy->incRefCount();
        ad->decRefCount();  // count > 1 to begin with; don't need release
        ad = copy;
      }
      Iter* it = FP2ITER(m_fp, itId);
      MIterCtx& mi = it->marr();
      (void) new (&mi) MIterCtx(ad);
      mi.m_mArray->advance();
      it->m_itype = Iter::TypeMutableArray;
    }
  } else {
    if (!hphpiCompat) {
      raise_warning("Invalid argument supplied for foreach()");
    }
    ITER_SKIP(offset);
  }
  m_stack.popV();
}

inline void OPTBLD_INLINE VMExecutionContext::iopIterValueC(PC& pc) {
  NEXT();
  DECODE_IA(itId);
  Iter* it = FP2ITER(m_fp, itId);
  switch (it->m_itype) {
  case Iter::TypeUndefined: {
    ASSERT(false);
    break;
  }
  case Iter::TypeArray:
  case Iter::TypeIterator: {
    // The emitter should never generate bytecode where the iterator
    // is at the end before IterValueC is executed. However, even if
    // the iterator is at the end, it is safe to call second().
    Cell* c1 = m_stack.allocC();
    TV_WRITE_NULL(c1);
    tvCellAsVariant(c1) = it->arr().second();
    break;
  }
  case Iter::TypeMutableArray: {
    // Dup value.
    TypedValue* tv1 = m_stack.allocTV();
    tvDup(&it->marr().m_val, tv1);
    ASSERT(tv1->m_type == KindOfVariant);
    tvUnbox(tv1);
    break;
  }
  default: {
    not_reached();
  }
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopIterValueV(PC& pc) {
  NEXT();
  DECODE_IA(itId);
  Iter* it = FP2ITER(m_fp, itId);
  switch (it->m_itype) {
  case Iter::TypeUndefined: {
    ASSERT(false);
    break;
  }
  case Iter::TypeArray:
  case Iter::TypeIterator: {
    // The emitter should never generate bytecode where the iterator
    // is at the end before IterValueV is executed. However, even if
    // the iterator is at the end, it is safe to call secondRef().
    TypedValue* tv = m_stack.allocTV();
    TV_WRITE_NULL(tv);
    tvAsVariant(tv) = ref(it->arr().secondRef());
    break;
  }
  case Iter::TypeMutableArray: {
    // Dup value.
    TypedValue* tv1 = m_stack.allocTV();
    tvDup(&it->marr().m_val, tv1);
    ASSERT(tv1->m_type == KindOfVariant);
    break;
  }
  default: {
    not_reached();
  }
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopIterKey(PC& pc) {
  NEXT();
  DECODE_IVA(itId);
  Iter* it = FP2ITER(m_fp, itId);
  switch (it->m_itype) {
  case Iter::TypeUndefined: {
    ASSERT(false);
    break;
  }
  case Iter::TypeArray:
  case Iter::TypeIterator: {
    // The iterator should never be at the end here. We can't check for it,
    // because that may call into user code, which has PHP-visible effects and
    // is incorrect.
    Cell* c1 = m_stack.allocC();
    TV_WRITE_NULL(c1);
    tvCellAsVariant(c1) = it->arr().first();
    break;
  }
  case Iter::TypeMutableArray: {
    // Dup key.
    TypedValue* tv1 = m_stack.allocTV();
    tvDup(&it->marr().m_key, tv1);
    if (tv1->m_type == KindOfVariant) {
      tvUnbox(tv1);
    }
    break;
  }
  default: {
    not_reached();
  }
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopIterNext(PC& pc) {
  PC origPc = pc;
  NEXT();
  DECODE_IA(itId);
  DECODE(Offset, offset);
  Iter* it = FP2ITER(m_fp, itId);
  switch (it->m_itype) {
  case Iter::TypeUndefined: {
    ASSERT(false);
    break;
  }
  case Iter::TypeArray:
  case Iter::TypeIterator: {
    // The emitter should never generate bytecode where the iterator
    // is at the end before IterNext is executed. However, even if
    // the iterator is at the end, it is safe to call next().
    if (iter_next_array(it)) {
      // If after advancing the iterator we have not reached the end,
      // jump to the location specified by the second immediate argument.
      ITER_SKIP(offset);
    } else {
      // If after advancing the iterator we have reached the end, free
      // the iterator and fall through to the next instruction.
      ASSERT(it->m_itype == Iter::TypeUndefined);
    }
    break;
  }
  case Iter::TypeMutableArray: {
    MIterCtx &mi = it->marr();
    if (!mi.m_mArray->advance()) {
      // If after advancing the iterator we have reached the end, free
      // the iterator and fall through to the next instruction.
      mi.~MIterCtx();
      it->m_itype = Iter::TypeUndefined;
    } else {
      // If after advancing the iterator we have not reached the end,
      // jump to the location specified by the second immediate argument.
      ITER_SKIP(offset);
    }
    break;
  }
  default: {
    not_reached();
  }
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopIterFree(PC& pc) {
  NEXT();
  DECODE_IA(itId);
  Iter* it = FP2ITER(m_fp, itId);
  switch (it->m_itype) {
  case Iter::TypeUndefined: {
    ASSERT(false);
    break;
  }
  case Iter::TypeArray:
  case Iter::TypeIterator: {
    it->arr().~ArrayIter();
    break;
  }
  case Iter::TypeMutableArray: {
    MIterCtx &mi = it->marr();
    mi.~MIterCtx();
    break;
  }
  default: {
    not_reached();
  }
  }
  it->m_itype = Iter::TypeUndefined;
}

inline void OPTBLD_INLINE inclOp(VMExecutionContext *ec, PC &pc, InclOpFlags flags) {
  NEXT();
  Cell* c1 = ec->m_stack.topC();
  String path(prepareKey(c1));
  bool initial;
  TRACE(2, "inclOp %s %s %s %s %s \"%s\"\n",
        flags & InclOpOnce ? "Once" : "",
        flags & InclOpDocRoot ? "DocRoot" : "",
        flags & InclOpRelative ? "Relative" : "",
        flags & InclOpLocal ? "Local" : "",
        flags & InclOpFatal ? "Fatal" : "",
        path->data());

  Unit* u = flags & (InclOpDocRoot|InclOpRelative) ?
    ec->evalIncludeRoot(path.get(), flags, &initial) :
    ec->evalInclude(path.get(), ec->m_fp->m_func->unit()->filepath(), &initial);
  ec->m_stack.popC();
  if (u == NULL) {
    ((flags & InclOpFatal) ?
     (void (*)(const char *, ...))raise_error :
     (void (*)(const char *, ...))raise_warning)("File not found: %s",
                                                 path->data());
    ec->m_stack.pushFalse();
  } else {
    if (!(flags & InclOpOnce) || initial) {
      ec->evalUnit(u, (flags & InclOpLocal), pc, EventHook::PseudoMain);
    } else {
      ec->m_stack.pushTrue();
    }
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopIncl(PC& pc) {
  inclOp(this, pc, InclOpDefault);
}

inline void OPTBLD_INLINE VMExecutionContext::iopInclOnce(PC& pc) {
  inclOp(this, pc, InclOpOnce);
}

inline void OPTBLD_INLINE VMExecutionContext::iopReq(PC& pc) {
  inclOp(this, pc, InclOpFatal);
}

inline void OPTBLD_INLINE VMExecutionContext::iopReqOnce(PC& pc) {
  inclOp(this, pc, InclOpFatal | InclOpOnce);
}

inline void OPTBLD_INLINE VMExecutionContext::iopReqDoc(PC& pc) {
  inclOp(this, pc, InclOpFatal | InclOpOnce | InclOpDocRoot);
}

inline void OPTBLD_INLINE VMExecutionContext::iopReqMod(PC& pc) {
  inclOp(this, pc, InclOpFatal | InclOpOnce | InclOpDocRoot | InclOpLocal);
}

inline void OPTBLD_INLINE VMExecutionContext::iopReqSrc(PC& pc) {
  inclOp(this, pc, InclOpFatal | InclOpOnce | InclOpRelative | InclOpLocal);
}

inline void OPTBLD_INLINE VMExecutionContext::iopEval(PC& pc) {
  NEXT();
  Cell* c1 = m_stack.topC();
  String code(prepareKey(c1));
  String prefixedCode = concat("<?php ", code);
  Unit* unit = compileEvalString(prefixedCode.get());
  if (unit == NULL) {
    raise_error("Syntax error in eval()");
  }
  m_stack.popC();
  evalUnit(unit, false, pc, EventHook::Eval);
}

inline void OPTBLD_INLINE VMExecutionContext::iopDefFunc(PC& pc) {
  NEXT();
  DECODE_IVA(fid);
  Func* f = m_fp->m_func->unit()->lookupFuncId(fid);
  f->setCached();
}

inline void OPTBLD_INLINE VMExecutionContext::iopDefCls(PC& pc) {
  NEXT();
  DECODE_IVA(cid);
  PreClass* c = m_fp->m_func->unit()->lookupPreClassId(cid);
  Unit::defClass(c);
}

inline void OPTBLD_INLINE VMExecutionContext::iopThis(PC& pc) {
  NEXT();
  if (!m_fp->hasThis()) {
    raise_error(Strings::FATAL_NULL_THIS);
  }
  ObjectData* this_ = m_fp->getThis();
  m_stack.pushObject(this_);
}

inline void OPTBLD_INLINE VMExecutionContext::iopInitThisLoc(PC& pc) {
  NEXT();
  DECODE_IVA(id);
  TypedValue* thisLoc = frame_local(m_fp, id);
  tvRefcountedDecRef(thisLoc);
  if (m_fp->hasThis()) {
    thisLoc->m_data.pobj = m_fp->getThis();
    thisLoc->_count = 0;
    thisLoc->m_type = KindOfObject;
    tvIncRef(thisLoc);
  } else {
    TV_WRITE_UNINIT(thisLoc);
  }
}

/*
 * Helper for StaticLoc and StaticLocInit.
 */
static inline void
lookupStatic(StringData* name,
             const ActRec* fp,
             TypedValue*&val, bool& inited) {
  HphpArray* map = get_static_locals(fp);
  ASSERT(map != NULL);
  val = map->nvGet(name);
  if (val == NULL) {
    TypedValue tv;
    TV_WRITE_UNINIT(&tv);
    map->nvSet(name, &tv, false);
    val = map->nvGet(name);
    inited = false;
  } else {
    inited = true;
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopStaticLoc(PC& pc) {
  NEXT();
  DECODE_IVA(localId);
  DECODE_LITSTR(var);
  TypedValue* fr = NULL;
  bool inited;
  lookupStatic(var, m_fp, fr, inited);
  ASSERT(fr != NULL);
  if (fr->m_type != KindOfVariant) {
    ASSERT(!inited);
    tvBox(fr);
  }
  TypedValue* tvLocal = frame_local(m_fp, localId);
  tvBind(fr, tvLocal);
  if (inited) {
    m_stack.pushTrue();
  } else {
    m_stack.pushFalse();
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopStaticLocInit(PC& pc) {
  NEXT();
  DECODE_IVA(localId);
  DECODE_LITSTR(var);
  TypedValue* fr = NULL;
  bool inited;
  lookupStatic(var, m_fp, fr, inited);
  ASSERT(fr != NULL);
  if (!inited) {
    Cell* initVal = m_stack.topC();
    tvDup(initVal, fr);
  }
  if (fr->m_type != KindOfVariant) {
    ASSERT(!inited);
    tvBox(fr);
  }
  TypedValue* tvLocal = frame_local(m_fp, localId);
  tvBind(fr, tvLocal);
  m_stack.discard();
}

inline void OPTBLD_INLINE VMExecutionContext::iopCatch(PC& pc) {
  NEXT();
  ASSERT(m_faults.size() > 0);
  Fault& fault = m_faults.back();
  ASSERT(fault.m_faultType == Fault::KindOfUserException);
  m_stack.pushObjectNoRc(fault.m_userException);
  m_faults.pop_back();
}

inline void OPTBLD_INLINE VMExecutionContext::iopLateBoundCls(PC& pc) {
  NEXT();
  m_stack.pushClass(frameStaticClass(m_fp));
}

inline void OPTBLD_INLINE VMExecutionContext::iopVerifyParamType(PC& pc) {
  SYNC(); // We might need m_pc to be updated to throw.
  NEXT();

  DECODE_IVA(param);
  const Func *func = m_fp->m_func;
  ASSERT(param < func->numParams());
  ASSERT(func->numParams() == int(func->params().size()));
  const TypeConstraint& tc = func->params()[param].typeConstraint();
  ASSERT(tc.exists());
  const TypedValue *tv = frame_local(m_fp, param);
  tc.verify(tv, func, param);
}

inline void OPTBLD_INLINE VMExecutionContext::iopNativeImpl(PC& pc) {
  NEXT();
  uint soff = m_fp->m_soff;
  BuiltinFunction func = m_fp->m_func->builtinFuncPtr();
  ASSERT(func);
  // Actually call the native implementation. This will handle freeing the
  // locals in the normal case. In the case of an exception, the VM unwinder
  // will take care of it.
  func(m_fp);
  // Adjust the stack; the native implementation put the return value in the
  // right place for us already
  m_stack.ndiscard(m_fp->m_func->numSlotsInFrame());
  ActRec* sfp = arGetSfp(m_fp);
  if (LIKELY(sfp != m_fp)) {
    // Restore caller's execution state.
    m_fp = sfp;
    pc = m_fp->m_func->unit()->entry() + m_fp->m_func->base() + soff;
    m_stack.ret();
  } else {
    // No caller; terminate.
    m_stack.ret();
#ifdef HPHP_TRACE
    {
      std::ostringstream os;
      m_stack.toStringElm(os, m_stack.topTV(), m_fp);
      ONTRACE(1,
              Trace::trace("Return %s from VMExecutionContext::dispatch("
                           "%p)\n", os.str().c_str(), m_fp));
    }
#endif
    pc = NULL;
    m_fp = NULL;
    g_vmContext->m_halted = true;
  }
}

inline void OPTBLD_INLINE VMExecutionContext::iopHighInvalid(PC& pc) {
  fprintf(stderr, "invalid bytecode executed\n");
  abort();
}

inline void OPTBLD_INLINE VMExecutionContext::iopSelf(PC& pc) {
  NEXT();
  Class* clss = arGetContextClass(m_fp);
  if (!clss) {
    raise_error(HPHP::Strings::CANT_ACCESS_SELF);
  }
  m_stack.pushClass(clss);
}

inline void OPTBLD_INLINE VMExecutionContext::iopParent(PC& pc) {
  NEXT();
  Class* clss = arGetContextClass(m_fp);
  if (!clss) {
    raise_error(HPHP::Strings::CANT_ACCESS_PARENT_WHEN_NO_CLASS);
  }
  Class* parent = clss->parent();
  if (!parent) {
    raise_error(HPHP::Strings::CANT_ACCESS_PARENT_WHEN_NO_PARENT);
  }
  m_stack.pushClass(parent);
}

c_GenericContinuation*
VMExecutionContext::createContinuation(ActRec* fp,
                                       bool getArgs,
                                       const Func* origFunc,
                                       Class* genClass,
                                       const Func* genFunc) {
  bool isMethod = origFunc->isNonClosureMethod();
  Object obj;
  Array args;
  if (fp->hasThis()) {
    obj = fp->getThis();
  }
  if (getArgs) {
    args = hhvm_get_frame_args(fp);
  }
  static const StringData* closure = StringData::GetStaticString("{closure}");
  const StringData* origName =
    origFunc->isClosureBody() ? closure : origFunc->fullName();
  c_GenericContinuation* cont =
    dynamic_cast<c_GenericContinuation*>(newInstance(genClass));
  ASSERT(cont);
  cont->create((int64)&ci_callUserFunc, (int64)genFunc, isMethod,
               StrNR(const_cast<StringData*>(origName)), Array(), obj, args);
  cont->incRefCount();
  cont->setNoDestruct();
  if (isMethod) {
    cont->m_vmCalledClass = (intptr_t)frameStaticClass(fp) | 0x1ll;
  }

  int nLocals = genFunc->numNamedLocals() - 1; //Don't need space for __cont__
  cont->m_locals = (TypedValue*)calloc(nLocals, sizeof(TypedValue));
  cont->m_nLocals = nLocals;
  return cont;
}

static inline void setContVar(const Func::PnameMap& pnames,
                              const StringData* name,
                              CVarRef value,
                              int nLocals,
                              c_GenericContinuation* cont) {
  Id id;
  Variant* dest;
  if (mapGet(pnames, name, &id)) {
    dest = &tvAsVariant(&cont->m_locals[nLocals - id]);
  } else {
    dest = &cont->m_vars.lval(*(String*)&name);
  }
  dest->setWithRef(value);
}

c_GenericContinuation*
VMExecutionContext::fillContinuationVars(ActRec* fp,
                                         const Func* origFunc,
                                         const Func* genFunc,
                                         c_GenericContinuation* cont) {
  // For functions that contain only named locals, the variable
  // environment is saved and restored by teleporting the values (and
  // their references) between the evaluation stack and m_locals using
  // memcpy. Any variables in a VarEnv are saved and restored from
  // m_vars as usual.
  static const StringData* thisStr = StringData::GetStaticString("this");
  int nLocals = cont->m_nLocals;
  const Func::PnameMap& genNames = genFunc->pnameMap();
  bool skipThis;
  if (fp->hasVarEnv()) {
    Stats::inc(Stats::Cont_CreateVerySlow);
    Array definedVariables = fp->getVarEnv()->getDefinedVariables();
    skipThis = definedVariables.exists("this", true);
    for (ArrayIter iter(definedVariables); !iter.end(); iter.next()) {
      setContVar(genNames, iter.first().getStringData(),
                 iter.secondRef(), nLocals, cont);
    }
  } else {
    const Func::PnameVec& varNames = origFunc->pnames();
    skipThis = mapContains(origFunc->pnameMap(), thisStr);
    for (unsigned i = 0; i < varNames.size(); ++i) {
      setContVar(genNames, varNames[i], tvAsCVarRef(frame_local(fp, i)),
                 nLocals, cont);
    }
  }
  cont->m_hasExtraVars = !cont->m_vars.empty();

  // If $this is used as a local inside the body and is not provided
  // by our containing environment, just prefill it here instead of
  // using InitThisLoc inside the body
  Id id;
  if (!skipThis && cont->m_obj.get() && mapGet(genNames, thisStr, &id)) {
    tvAsVariant(&cont->m_locals[nLocals - id]) = cont->m_obj;
  }
  return cont;
}

inline void OPTBLD_INLINE VMExecutionContext::iopCreateCont(PC& pc) {
  NEXT();
  DECODE_IVA(getArgs);
  DECODE_LITSTR(genName);
  DECODE_LITSTR(className);

  const Func* origFunc = m_fp->m_func;
  const Func* genFunc = origFunc->getGeneratorBody(genName);
  ASSERT(genFunc != NULL);

  Class* cls = Unit::lookupClass(className);
  ASSERT(cls);
  c_GenericContinuation* cont =
    createContinuation(m_fp, getArgs, origFunc, cls, genFunc);
  fillContinuationVars(m_fp, origFunc, genFunc, cont);

  TypedValue* ret = m_stack.allocTV();
  ret->m_type = KindOfObject;
  ret->_count = 0;
  ret->m_data.pobj = cont;
}

static inline c_GenericContinuation* frame_continuation(ActRec* fp) {
  c_GenericContinuation* cont =
    dynamic_cast<c_GenericContinuation*>(frame_local(fp, 0)->m_data.pobj);
  ASSERT(cont != NULL);
  return cont;
}

int VMExecutionContext::unpackContinuation(c_GenericContinuation* cont,
                                           TypedValue* dest) {
  // Teleport the references that live in the continuation object to
  // the runtime stack so we don't have to do any refcounting in the
  // fast case
  int nCopy = cont->m_nLocals;
  TypedValue* src = &cont->m_locals[0];
  memcpy(dest, src, nCopy * sizeof(TypedValue));
  memset(src, 0x0, nCopy * sizeof(TypedValue));

  ASSERT(cont->m_hasExtraVars == !cont->m_vars.empty());
  if (UNLIKELY(cont->m_hasExtraVars)) {
    Stats::inc(Stats::Cont_UnpackVerySlow);
    VarEnv* env = g_vmContext->getVarEnv(false);
    for (ArrayIter iter(cont->m_vars); !iter.end(); iter.next()) {
      StringData* key = iter.first().getStringData();
      TypedValue* value = iter.secondRef().getTypedAccessor();
      env->setWithRef(key, value);
    }
    cont->m_vars.clear();
  }

  return cont->m_label;
}

inline void OPTBLD_INLINE VMExecutionContext::iopUnpackCont(PC& pc) {
  NEXT();
  ASSERT(!m_fp->hasVarEnv());
  c_GenericContinuation* cont = frame_continuation(m_fp);

  int nCopy = m_fp->m_func->numNamedLocals() - 1;
  int label = unpackContinuation(cont, frame_local(m_fp, nCopy));

  // Return the label in a stack cell
  TypedValue* ret = m_stack.allocTV();
  ret->m_type = KindOfInt64;
  ret->_count = 0;
  ret->m_data.num = label;
}

void VMExecutionContext::packContinuation(c_GenericContinuation* cont,
                                          ActRec* fp,
                                          TypedValue* value,
                                          int label) {
  int nCopy = cont->m_nLocals;
  TypedValue* src = frame_local(fp, nCopy);
  TypedValue* dest = &cont->m_locals[0];
  memcpy(dest, src, nCopy * sizeof(TypedValue));
  memset(src, 0x0, nCopy * sizeof(TypedValue));

  // If we have a varEnv, stick any non-named locals into cont->m_vars
  if (UNLIKELY(fp->hasVarEnv())) {
    Stats::inc(Stats::Cont_PackVerySlow);
    const Func* f = fp->m_func;
    ASSERT(f != NULL);
    ASSERT(cont->m_vars.empty());
    Array vars = fp->getVarEnv()->getDefinedVariables();
    for (ArrayIter iter(vars); !iter.end(); iter.next()) {
      Variant key = iter.first();
      CVarRef value = iter.secondRef();
      if (!mapContains(f->pnameMap(), key.getStringData())) {
        if (value.m_type == KindOfVariant) {
          cont->m_vars.setRef(key, value);
        } else {
          cont->m_vars.add(key, value);
        }
      }
    }
    cont->m_hasExtraVars = !cont->m_vars.empty();
  }

  cont->c_Continuation::t_update(label, tvAsCVarRef(value));
}

inline void OPTBLD_INLINE VMExecutionContext::iopPackCont(PC& pc) {
  NEXT();
  DECODE_IVA(label);
  c_GenericContinuation* cont = frame_continuation(m_fp);

  packContinuation(cont, m_fp, m_stack.topTV(), label);
  m_stack.popTV();
}

inline void OPTBLD_INLINE VMExecutionContext::iopContReceive(PC& pc) {
  NEXT();
  c_GenericContinuation* cont = frame_continuation(m_fp);
  Variant val = cont->t_receive();

  TypedValue* tv = m_stack.allocTV();
  TV_WRITE_UNINIT(tv);
  tvAsVariant(tv) = val;
}

inline void OPTBLD_INLINE VMExecutionContext::iopContRaised(PC& pc) {
  NEXT();
  c_GenericContinuation* cont = frame_continuation(m_fp);
  cont->t_raised();
}

inline void OPTBLD_INLINE VMExecutionContext::iopContDone(PC& pc) {
  NEXT();
  c_GenericContinuation* cont = frame_continuation(m_fp);
  cont->t_done();
}

static inline c_GenericContinuation* this_continuation(ActRec* fp) {
  c_GenericContinuation* c =
    dynamic_cast<c_GenericContinuation*>(fp->getThis());
  ASSERT(c != NULL);
  return c;
}

inline void OPTBLD_INLINE VMExecutionContext::iopFPushContFunc(PC& pc) {
  NEXT();
  DECODE_IVA(nArgs);
  ASSERT(nArgs == 1);

  c_GenericContinuation* cont = this_continuation(m_fp);
  Func* func = cont->m_vmFunc;
  func->validate();
  ActRec* ar = m_stack.allocA();
  arSetSfp(ar, m_fp);
  ar->m_func = func;
  if (cont->m_isMethod) {
    if (cont->m_obj.get()) {
      ar->setThis(cont->m_obj.get());
      cont->m_obj.get()->incRefCount();
    } else {
      // m_vmCalledClass already has its low bit set so we want to
      // bypass setClass().
      ar->m_cls = (Class*)cont->m_vmCalledClass;
    }
  } else {
    ar->setThis(NULL);
  }
  ar->initNumArgs(1);
  ar->setVarEnv(NULL);
}

inline void OPTBLD_INLINE VMExecutionContext::iopContNext(PC& pc) {
  NEXT();
  c_GenericContinuation* cont = this_continuation(m_fp);
  cont->m_received.setNull();
  cont->preNext();

  TypedValue* tv = m_stack.allocTV();
  TV_WRITE_UNINIT(tv);
  tvAsVariant(tv) = cont;
}

template<bool raise>
inline void VMExecutionContext::contSendImpl() {
  c_GenericContinuation* cont = this_continuation(m_fp);
  cont->nextCheck();
  cont->m_received.assignVal(tvAsVariant(frame_local(m_fp, 0)));
  if (raise) {
    cont->m_should_throw = true;
  }
  cont->preNext();

  TypedValue* tv = m_stack.allocTV();
  TV_WRITE_UNINIT(tv);
  tvAsVariant(tv) = cont;
}

inline void OPTBLD_INLINE VMExecutionContext::iopContSend(PC& pc) {
  NEXT();
  contSendImpl<false>();
}

inline void OPTBLD_INLINE VMExecutionContext::iopContRaise(PC& pc) {
  NEXT();
  contSendImpl<true>();
}

inline void OPTBLD_INLINE VMExecutionContext::iopContValid(PC& pc) {
  NEXT();
  TypedValue* tv = m_stack.allocTV();
  TV_WRITE_UNINIT(tv);
  tvAsVariant(tv) = !this_continuation(m_fp)->m_done;
}

inline void OPTBLD_INLINE VMExecutionContext::iopContCurrent(PC& pc) {
  NEXT();
  c_GenericContinuation* cont = this_continuation(m_fp);
  cont->nextCheck();

  TypedValue* tv = m_stack.allocTV();
  TV_WRITE_UNINIT(tv);
  tvAsVariant(tv) = cont->m_value;
}

inline void OPTBLD_INLINE VMExecutionContext::iopContStopped(PC& pc) {
  NEXT();
  this_continuation(m_fp)->m_running = false;
}

inline void OPTBLD_INLINE VMExecutionContext::iopContHandle(PC& pc) {
  NEXT();
  c_GenericContinuation* cont = this_continuation(m_fp);
  cont->m_running = false;
  cont->m_done = true;

  Variant exn = tvAsVariant(m_stack.topTV());
  m_stack.popC();
  ASSERT(exn.asObjRef().instanceof("exception"));
  throw exn.asObjRef();
}

void VMExecutionContext::classExistsImpl(PC& pc, Attr typeAttr) {
  NEXT();
  TypedValue* aloadTV = m_stack.topTV();
  tvCastToBooleanInPlace(aloadTV);
  ASSERT(aloadTV->m_type == KindOfBoolean);
  bool autoload = aloadTV->m_data.num;
  m_stack.popX();

  TypedValue* name = m_stack.topTV();
  tvCastToStringInPlace(name);
  ASSERT(IS_STRING_TYPE(name->m_type));

  tvAsVariant(name) = Unit::classExists(name->m_data.pstr, autoload, typeAttr);
}

inline void OPTBLD_INLINE VMExecutionContext::iopClassExists(PC& pc) {
  classExistsImpl(pc, AttrNone);
}

inline void OPTBLD_INLINE VMExecutionContext::iopInterfaceExists(PC& pc) {
  classExistsImpl(pc, AttrInterface);
}

inline void OPTBLD_INLINE VMExecutionContext::iopTraitExists(PC& pc) {
  classExistsImpl(pc, AttrTrait);
}

string
VMExecutionContext::prettyStack(const string& prefix) const {
  if (m_halted) {
    string s("__Halted");
    return s;
  }
  int offset = (m_fp->m_func->unit() != NULL)
               ? pcOff()
               : 0;
  string begPrefix = prefix + "__";
  string midPrefix = prefix + "|| ";
  string endPrefix = prefix + "\\/";
  string stack = m_stack.toString(m_fp, offset, midPrefix);
  return begPrefix + "\n" + stack + endPrefix;
}

void VMExecutionContext::checkRegStateWork() const {
  ASSERT(tl_regState == REGSTATE_CLEAN);
}

void VMExecutionContext::DumpStack() {
  string s = g_vmContext->prettyStack("");
  fprintf(stderr, "%s\n", s.c_str());
}

void VMExecutionContext::DumpCurUnit(int skip) {
  ActRec* fp = g_vmContext->m_fp;
  Offset pc = fp->m_func->unit() ? g_vmContext->pcOff() : 0;
  while (skip--) {
    fp = g_vmContext->getPrevVMState(fp, &pc);
  }
  if (fp == NULL) {
    std::cout << "Don't have a valid fp\n";
    return;
  }

  printf("Offset = %d, in function %s\n", pc, fp->m_func->name()->data());
  Unit* u = fp->m_func->unit();
  if (u == NULL) {
    std::cout << "Current unit is NULL\n";
    return;
  }
  printf("Dumping bytecode for %s(%p)\n", u->filepath()->data(), u);
  std::cout << u->toString();
}

static inline void
condStackTraceSep(const char* pfx) {
  TRACE(3, "%s"
        "========================================"
        "========================================\n",
        pfx);
}

#define COND_STACKTRACE(pfx)                                                  \
  ONTRACE(3,                                                                  \
          string stack = prettyStack(pfx);                                    \
          Trace::trace("%s\n", stack.c_str());)

#define O(name, imm, pusph, pop, flags)                                       \
void VMExecutionContext::op##name() {                                         \
  condStackTraceSep("op"#name" ");                                            \
  COND_STACKTRACE("op"#name" pre:  ");                                        \
  PC pc = m_pc;                                                               \
  ASSERT(*pc == Op##name);                                                    \
  ONTRACE(1,                                                                  \
          int offset = m_fp->m_func->unit()->offsetOf(pc);                    \
          Trace::trace("op"#name" offset: %d\n", offset));                    \
  iop##name(pc);                                                              \
  SYNC();                                                                     \
  COND_STACKTRACE("op"#name" post: ");                                        \
  condStackTraceSep("op"#name" ");                                            \
}
OPCODES
#undef O
#undef NEXT
#undef DECODE_JMP
#undef DECODE

template <bool limInstrs, bool breakOnCtlFlow>
inline void VMExecutionContext::dispatchImpl(int numInstrs) {
  static const void *optabDirect[] = {
#define O(name, imm, push, pop, flags) \
    &&Label##name,
    OPCODES
#undef O
  };
  static const void *optabDbg[] = {
#define O(name, imm, push, pop, flags) \
    &&LabelDbg##name,
    OPCODES
#undef O
  };
  static const void *optabInst[] __attribute__((unused)) = {
#define O(name, imm, push, pop, flags) \
    &&LabelInst##name,
    OPCODES
#undef O
  };
  static const void *optabCover[] = {
#define O(name, imm, push, pop, flags) \
    &&LabelCover##name,
    OPCODES
#undef O
  };
  ASSERT(sizeof(optabDirect) / sizeof(const void *) == Op_count);
  ASSERT(sizeof(optabDbg) / sizeof(const void *) == Op_count);
  const void **optab = optabDirect;
  InjectionTableInt64* injTable = g_vmContext->m_injTables ?
    g_vmContext->m_injTables->getInt64Table(InstHookTypeBCPC) : NULL;
  bool collectCoverage = ThreadInfo::s_threadInfo->m_reqInjectionData.coverage;
  if (injTable) {
    optab = optabInst;
  } else if (collectCoverage) {
    optab = optabCover;
  }
  DEBUGGER_ATTACHED_ONLY(optab = optabDbg);
  /*
   * Trace-only mapping of opcodes to names.
   */
#ifdef HPHP_TRACE
  static const char *nametab[] = {
#define O(name, imm, push, pop, flags) \
    #name,
    OPCODES
#undef O
  };
#endif /* HPHP_TRACE */
  bool isCtlFlow = false;

#define DISPATCH() do {                                                       \
    if ((breakOnCtlFlow && isCtlFlow) ||                                      \
        (limInstrs && UNLIKELY(numInstrs-- == 0))) {                          \
      ONTRACE(1,                                                              \
              Trace::trace("dispatch: Halt ExecutionContext::dispatch(%p)\n", \
                           m_fp));                                            \
      delete g_vmContext->m_lastLocFilter;                                    \
      g_vmContext->m_lastLocFilter = NULL;                                    \
      return;                                                                 \
    }                                                                         \
    Op op = (Op)*pc;                                                          \
    COND_STACKTRACE("dispatch:                    ");                         \
    ONTRACE(1,                                                                \
            Trace::trace("dispatch: %d: %s\n", pcOff(), nametab[op]));        \
    ASSERT(op < Op_count);                                                    \
    goto *optab[op];                                                          \
} while (0)

  ONTRACE(1, Trace::trace("dispatch: Enter ExecutionContext::dispatch(%p)\n",
          m_fp));
  PC pc = m_pc;
  DISPATCH();

#define O(name, imm, pusph, pop, flags)                                       \
  LabelDbg##name:                                                             \
    phpDebuggerHook(pc);                                                      \
  LabelInst##name:                                                            \
    INST_HOOK_PC(injTable, pc);                                               \
  LabelCover##name:                                                           \
    if (collectCoverage) {                                                    \
      recordCodeCoverage(pc);                                                 \
    }                                                                         \
  Label##name: {                                                              \
    iop##name(pc);                                                            \
    SYNC();                                                                   \
    if (g_vmContext->m_halted) {                                              \
      return;                                                                 \
    }                                                                         \
    if (breakOnCtlFlow) {                                                     \
      isCtlFlow = instrIsControlFlow(Op##name);                               \
      Stats::incOp(Op##name);                                                 \
    }                                                                         \
    DISPATCH();                                                               \
  }
  OPCODES
#undef O
#undef DISPATCH
}

class InterpretingFlagGuard {
private:
  bool m_oldFlag;
public:
  InterpretingFlagGuard() {
    m_oldFlag = g_vmContext->m_interpreting;
    g_vmContext->m_interpreting = true;
  }
  ~InterpretingFlagGuard() {
    g_vmContext->m_interpreting = m_oldFlag;
  }
};

void VMExecutionContext::dispatch() {
  InterpretingFlagGuard ifg;
  dispatchImpl<false, false>(0);
}

void VMExecutionContext::dispatchN(int numInstrs) {
  InterpretingFlagGuard ifg;
  dispatchImpl<true, false>(numInstrs);
  // We are about to go back to Jit, check whether we should
  // stick with interpreter
  if (DEBUGGER_FORCE_INTR) {
    throw VMSwitchModeException(false);
  }
}

void VMExecutionContext::dispatchBB() {
  InterpretingFlagGuard ifg;
  dispatchImpl<false, true>(0);
  // We are about to go back to Jit, check whether we should
  // stick with interpreter
  if (DEBUGGER_FORCE_INTR) {
    throw VMSwitchModeException(false);
  }
}

void VMExecutionContext::recordCodeCoverage(PC pc) {
  Unit* unit = m_fp->m_func->unit();
  ASSERT(unit != NULL);
  if (unit == SystemLib::s_nativeFuncUnit ||
      unit == SystemLib::s_nativeClassUnit) {
    return;
  }
  int line = unit->getLineNumber(pcOff());
  ASSERT(line != -1);

  if (unit != m_coverPrevUnit || line != m_coverPrevLine) {
    ThreadInfo* info = ThreadInfo::s_threadInfo.getNoCheck();
    m_coverPrevUnit = unit;
    m_coverPrevLine = line;
    const StringData* filepath = unit->filepath();
    ASSERT(filepath->isStatic());
    info->m_coverage->Record(filepath->data(), line, line);
  }
}

void VMExecutionContext::resetCoverageCounters() {
  m_coverPrevLine = -1;
  m_coverPrevUnit = NULL;
}

void VMExecutionContext::pushVMState(VMState &savedVM) {
  if (debug && savedVM.fp &&
      savedVM.fp->m_func &&
      savedVM.fp->m_func->unit()) {
    // Some asserts and tracing.
    const Func* func = savedVM.fp->m_func;
    (void) /* bound-check asserts in offsetOf */
      func->unit()->offsetOf(savedVM.pc);
    TRACE(3, "pushVMState: saving frame %s pc %p off %d fp %p\n",
          func->name()->data(),
          savedVM.pc,
          func->unit()->offsetOf(savedVM.pc),
          savedVM.fp);
  }
  m_nestedVMs.push_back(savedVM);
  m_nesting++;
}

void VMExecutionContext::popVMState() {
  ASSERT(m_nestedVMs.size() >= 1);
  if (debug) {
    const VMState& savedVM = m_nestedVMs.back();
    if (savedVM.fp &&
        savedVM.fp->m_func &&
        savedVM.fp->m_func->unit()) {
      const Func* func = savedVM.fp->m_func;
      (void) /* bound-check asserts in offsetOf */
        func->unit()->offsetOf(savedVM.pc);
      TRACE(3, "popVMState: restoring frame %s pc %p off %d fp %p\n",
            func->name()->data(),
            savedVM.pc,
            func->unit()->offsetOf(savedVM.pc),
            savedVM.fp);
    }
  }
  m_nestedVMs.pop_back();
  m_nesting--;
}

void VMExecutionContext::requestInit() {
  const_assert(hhvm);

  ASSERT(SystemLib::s_unit);
  ASSERT(SystemLib::s_nativeFuncUnit);
  ASSERT(SystemLib::s_nativeClassUnit);

  m_stack.requestInit();
  m_transl->requestInit();

  // Merge the systemlib unit into the ExecutionContext
  mergeUnit(SystemLib::s_unit);
  mergeUnit(SystemLib::s_nativeFuncUnit);
  mergeUnit(SystemLib::s_nativeClassUnit);

#ifdef DEBUG
  Class *cls = *Unit::GetNamedEntity(s_stdclass.get())->clsList();
  ASSERT(cls);
  ASSERT(cls == SystemLib::s_stdclassClass);
#endif
}

void VMExecutionContext::requestExit() {
  const_assert(hhvm);
  destructObjects();
  m_transl->requestExit();
  m_stack.requestExit();
  EventHook::Disable();
}

///////////////////////////////////////////////////////////////////////////////
}
