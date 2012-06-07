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

#ifndef __TEST_EXT_FB_H__
#define __TEST_EXT_FB_H__

// >>>>>> Generated by idl.php. Do NOT modify. <<<<<<

#include <test/test_cpp_ext.h>

///////////////////////////////////////////////////////////////////////////////

class TestExtFb : public TestCppExt {
 public:
  virtual bool RunTests(const std::string &which);

  bool test_fb_thrift_serialize();
  bool test_fb_thrift_unserialize();
  bool test_fb_rename_function();
  bool test_fb_utf8ize();
  bool test_fb_utf8_strlen();
  bool test_fb_utf8_strlen_deprecated();
  bool test_fb_utf8_substr();
  bool test_fb_call_user_func_safe();
  bool test_fb_call_user_func_safe_return();
  bool test_fb_call_user_func_array_safe();
  bool test_fb_load_local_databases();
  bool test_fb_parallel_query();
  bool test_fb_crossall_query();
};

///////////////////////////////////////////////////////////////////////////////

#endif // __TEST_EXT_FB_H__
