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
// @generated by HipHop Compiler

#ifndef __GENERATED_cls_SplObjectStorage_h3ad1ae4b__
#define __GENERATED_cls_SplObjectStorage_h3ad1ae4b__

#include <runtime/base/hphp_system.h>
#include <system/gen/sys/literal_strings_remap.h>
#include <system/gen/sys/scalar_arrays_remap.h>
#include <cls/Iterator.h>
#include <cls/Countable.h>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

/* SRC: classes/splobjectstorage.php line 12 */
FORWARD_DECLARE_CLASS(SplObjectStorage);
extern const ObjectStaticCallbacks cw_SplObjectStorage;
class c_SplObjectStorage : public ExtObjectData {
  public:

  // Properties
  Variant m_storage;
  int64 m_index;

  // Destructor
  ~c_SplObjectStorage() NEVER_INLINE {}
  // Class Map
  DECLARE_CLASS_NO_SWEEP(SplObjectStorage, SplObjectStorage, ObjectData)
  static const ClassPropTable os_prop_table;
  c_SplObjectStorage(const ObjectStaticCallbacks *cb = &cw_SplObjectStorage) : ExtObjectData(cb), m_index(0LL) {
    if (!hhvm) setAttribute(NoDestructor);
  }
  void init();
  public: void t_rewind();
  public: bool t_valid();
  public: int64 t_key();
  public: Variant t_current();
  public: void t_next();
  public: int t_count();
  public: bool t_contains(CVarRef v_obj);
  public: void t_attach(CVarRef v_obj);
  public: void t_detach(CVarRef v_obj);
  DECLARE_METHOD_INVOKE_HELPERS(rewind);
  DECLARE_METHOD_INVOKE_HELPERS(valid);
  DECLARE_METHOD_INVOKE_HELPERS(key);
  DECLARE_METHOD_INVOKE_HELPERS(current);
  DECLARE_METHOD_INVOKE_HELPERS(next);
  DECLARE_METHOD_INVOKE_HELPERS(count);
  DECLARE_METHOD_INVOKE_HELPERS(contains);
  DECLARE_METHOD_INVOKE_HELPERS(attach);
  DECLARE_METHOD_INVOKE_HELPERS(detach);
};
ObjectData *coo_SplObjectStorage() NEVER_INLINE;

///////////////////////////////////////////////////////////////////////////////
}

#endif // __GENERATED_cls_SplObjectStorage_h3ad1ae4b__
