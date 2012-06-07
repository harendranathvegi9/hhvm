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

#ifndef __GENERATED_cls_stdClass_hd2e0228e__
#define __GENERATED_cls_stdClass_hd2e0228e__

#include <runtime/base/hphp_system.h>
#include <system/gen/sys/literal_strings_remap.h>
#include <system/gen/sys/scalar_arrays_remap.h>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

/* SRC: classes/stdclass.php line 4 */
FORWARD_DECLARE_CLASS(stdClass);
extern const ObjectStaticCallbacks cw_stdClass;
class c_stdClass : public ExtObjectData {
  public:

  // Properties

  // Class Map
  DECLARE_CLASS_COMMON_NO_SWEEP(stdClass, stdClass)
  c_stdClass(const ObjectStaticCallbacks *cb = &cw_stdClass) : ExtObjectData(cb) {
    if (!hhvm) setAttribute(NoDestructor);
  }
};
ObjectData *coo_stdClass() NEVER_INLINE;

///////////////////////////////////////////////////////////////////////////////
}

#endif // __GENERATED_cls_stdClass_hd2e0228e__
