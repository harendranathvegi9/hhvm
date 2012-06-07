/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   | Copyright (c) 1997-2010 The PHP Group                                |
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

#ifndef __EXT_CLOSURE_H__
#define __EXT_CLOSURE_H__

// >>>>>> Generated by idl.php. Do NOT modify. <<<<<<

#include <runtime/base/base_includes.h>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////
// class Closure

FORWARD_DECLARE_CLASS_BUILTIN(Closure);
class c_Closure : public ExtObjectData {
 public:
  DECLARE_CLASS(Closure, Closure, ObjectData)

  // need to implement
  public: c_Closure(const ObjectStaticCallbacks *cb = &cw_Closure);
  public: ~c_Closure();
  public: void t___construct();
  DECLARE_METHOD_INVOKE_HELPERS(__construct);
  public: Variant t___invoke(int _argc, CArrRef _argv = null_array);
  DECLARE_METHOD_INVOKE_HELPERS(__invoke);
  public: Variant t___clone();
  DECLARE_METHOD_INVOKE_HELPERS(__clone);
  public: Variant t___destruct();
  DECLARE_METHOD_INVOKE_HELPERS(__destruct);

  // implemented by HPHP
  public: c_Closure *create();
public:
  /**
   * Explicitly provide a t___invokeCallInfoHelper to
   * allow __invoke() to sidestep an extra level of indirection
   */
  virtual const CallInfo *t___invokeCallInfoHelper(void *&extra);

  /**
   * Used by HPHPI to make closures work
   */
  void *extraData() const { return m_extraData; }

  /**
   * This is the constructor which is called internally-
   * PHP code will never be able to call this constructor
   */
  c_Closure(const CallInfo *callInfo, void *extraData,
            const ObjectStaticCallbacks *cb = &cw_Closure) :
      ExtObjectData(cb), m_callInfo(callInfo), m_extraData(extraData) {
    ASSERT(callInfo);
  }
protected:
  virtual bool php_sleep(Variant &ret);
private:
  const CallInfo *m_callInfo;
  void *m_extraData;
};

///////////////////////////////////////////////////////////////////////////////
// class GeneratorClosure

FORWARD_DECLARE_CLASS_BUILTIN(GeneratorClosure);
class c_GeneratorClosure : public c_Closure {
 public:
  // Closure is deliberately passed in as the original name,
  // so that get_class() on a GeneratorClosure returns
  // the string "Closure"
  DECLARE_CLASS(GeneratorClosure, Closure, Closure)

  // need to implement
  public: c_GeneratorClosure(const ObjectStaticCallbacks *cb = &cw_GeneratorClosure);
  public: ~c_GeneratorClosure();
  public: void t___construct();
  DECLARE_METHOD_INVOKE_HELPERS(__construct);
  public: Variant t___destruct();
  DECLARE_METHOD_INVOKE_HELPERS(__destruct);

  // implemented by HPHP
  public: c_GeneratorClosure *create();
public:
  /**
   * This is the constructor which is called internally-
   * PHP code will never be able to call this constructor
   */
  c_GeneratorClosure(
    const CallInfo *callInfo,
    void *extraData,
    CArrRef vars) :
    c_Closure(callInfo, extraData, &cw_GeneratorClosure), m_vars(vars) {}
public:
  Array          m_vars;    /* use variables */
  LVariableTable m_statics; /* static variables */
};

///////////////////////////////////////////////////////////////////////////////
// class DummyClosure

FORWARD_DECLARE_CLASS_BUILTIN(DummyClosure);
class c_DummyClosure : public ExtObjectData {
 public:
  DECLARE_CLASS(DummyClosure, DummyClosure, ObjectData)

  // need to implement
  public: c_DummyClosure(const ObjectStaticCallbacks *cb = &cw_DummyClosure);
  public: ~c_DummyClosure();
  public: void t___construct();
  DECLARE_METHOD_INVOKE_HELPERS(__construct);
  public: Variant t___destruct();
  DECLARE_METHOD_INVOKE_HELPERS(__destruct);

  // implemented by HPHP
  public: c_DummyClosure *create();

};

///////////////////////////////////////////////////////////////////////////////
}

#endif // __EXT_CLOSURE_H__
