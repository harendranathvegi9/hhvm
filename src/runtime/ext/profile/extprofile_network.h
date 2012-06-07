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

#ifndef __EXTPROFILE_NETWORK_H__
#define __EXTPROFILE_NETWORK_H__

// >>>>>> Generated by idl.php. Do NOT modify. <<<<<<

#include <runtime/ext/ext_network.h>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

inline Variant x_gethostname() {
  FUNCTION_INJECTION_BUILTIN(gethostname);
  return f_gethostname();
}

inline Variant x_gethostbyaddr(CStrRef ip_address) {
  FUNCTION_INJECTION_BUILTIN(gethostbyaddr);
  return f_gethostbyaddr(ip_address);
}

inline String x_gethostbyname(CStrRef hostname) {
  FUNCTION_INJECTION_BUILTIN(gethostbyname);
  return f_gethostbyname(hostname);
}

inline Variant x_gethostbynamel(CStrRef hostname) {
  FUNCTION_INJECTION_BUILTIN(gethostbynamel);
  return f_gethostbynamel(hostname);
}

inline Variant x_getprotobyname(CStrRef name) {
  FUNCTION_INJECTION_BUILTIN(getprotobyname);
  return f_getprotobyname(name);
}

inline Variant x_getprotobynumber(int number) {
  FUNCTION_INJECTION_BUILTIN(getprotobynumber);
  return f_getprotobynumber(number);
}

inline Variant x_getservbyname(CStrRef service, CStrRef protocol) {
  FUNCTION_INJECTION_BUILTIN(getservbyname);
  return f_getservbyname(service, protocol);
}

inline Variant x_getservbyport(int port, CStrRef protocol) {
  FUNCTION_INJECTION_BUILTIN(getservbyport);
  return f_getservbyport(port, protocol);
}

inline Variant x_inet_ntop(CStrRef in_addr) {
  FUNCTION_INJECTION_BUILTIN(inet_ntop);
  return f_inet_ntop(in_addr);
}

inline Variant x_inet_pton(CStrRef address) {
  FUNCTION_INJECTION_BUILTIN(inet_pton);
  return f_inet_pton(address);
}

inline Variant x_ip2long(CStrRef ip_address) {
  FUNCTION_INJECTION_BUILTIN(ip2long);
  return f_ip2long(ip_address);
}

inline String x_long2ip(int proper_address) {
  FUNCTION_INJECTION_BUILTIN(long2ip);
  return f_long2ip(proper_address);
}

inline bool x_dns_check_record(CStrRef host, CStrRef type = null_string) {
  FUNCTION_INJECTION_BUILTIN(dns_check_record);
  return f_dns_check_record(host, type);
}

inline bool x_checkdnsrr(CStrRef host, CStrRef type = null_string) {
  FUNCTION_INJECTION_BUILTIN(checkdnsrr);
  return f_checkdnsrr(host, type);
}

inline Variant x_dns_get_record(CStrRef hostname, int type = -1, VRefParam authns = null, VRefParam addtl = null) {
  FUNCTION_INJECTION_BUILTIN(dns_get_record);
  return f_dns_get_record(hostname, type, authns, addtl);
}

inline bool x_dns_get_mx(CStrRef hostname, VRefParam mxhosts, VRefParam weights = null) {
  FUNCTION_INJECTION_BUILTIN(dns_get_mx);
  return f_dns_get_mx(hostname, mxhosts, weights);
}

inline bool x_getmxrr(CStrRef hostname, VRefParam mxhosts, VRefParam weight = null) {
  FUNCTION_INJECTION_BUILTIN(getmxrr);
  return f_getmxrr(hostname, mxhosts, weight);
}

inline Variant x_fsockopen(CStrRef hostname, int port = -1, VRefParam errnum = null, VRefParam errstr = null, double timeout = 0.0) {
  FUNCTION_INJECTION_BUILTIN(fsockopen);
  return f_fsockopen(hostname, port, errnum, errstr, timeout);
}

inline Variant x_pfsockopen(CStrRef hostname, int port = -1, VRefParam errnum = null, VRefParam errstr = null, double timeout = 0.0) {
  FUNCTION_INJECTION_BUILTIN(pfsockopen);
  return f_pfsockopen(hostname, port, errnum, errstr, timeout);
}

inline Variant x_socket_get_status(CObjRef stream) {
  FUNCTION_INJECTION_BUILTIN(socket_get_status);
  return f_socket_get_status(stream);
}

inline bool x_socket_set_blocking(CObjRef stream, int mode) {
  FUNCTION_INJECTION_BUILTIN(socket_set_blocking);
  return f_socket_set_blocking(stream, mode);
}

inline bool x_socket_set_timeout(CObjRef stream, int seconds, int microseconds = 0) {
  FUNCTION_INJECTION_BUILTIN(socket_set_timeout);
  return f_socket_set_timeout(stream, seconds, microseconds);
}

inline void x_header(CStrRef str, bool replace = true, int http_response_code = 0) {
  FUNCTION_INJECTION_BUILTIN(header);
  f_header(str, replace, http_response_code);
}

inline Array x_headers_list() {
  FUNCTION_INJECTION_BUILTIN(headers_list);
  return f_headers_list();
}

inline bool x_headers_sent(VRefParam file = null, VRefParam line = null) {
  FUNCTION_INJECTION_BUILTIN(headers_sent);
  return f_headers_sent(file, line);
}

inline void x_header_remove(CStrRef name = null_string) {
  FUNCTION_INJECTION_BUILTIN(header_remove);
  f_header_remove(name);
}

inline bool x_setcookie(CStrRef name, CStrRef value = null_string, int64 expire = 0, CStrRef path = null_string, CStrRef domain = null_string, bool secure = false, bool httponly = false) {
  FUNCTION_INJECTION_BUILTIN(setcookie);
  return f_setcookie(name, value, expire, path, domain, secure, httponly);
}

inline bool x_setrawcookie(CStrRef name, CStrRef value = null_string, int64 expire = 0, CStrRef path = null_string, CStrRef domain = null_string, bool secure = false, bool httponly = false) {
  FUNCTION_INJECTION_BUILTIN(setrawcookie);
  return f_setrawcookie(name, value, expire, path, domain, secure, httponly);
}

inline void x_define_syslog_variables() {
  FUNCTION_INJECTION_BUILTIN(define_syslog_variables);
  f_define_syslog_variables();
}

inline bool x_openlog(CStrRef ident, int option, int facility) {
  FUNCTION_INJECTION_BUILTIN(openlog);
  return f_openlog(ident, option, facility);
}

inline bool x_closelog() {
  FUNCTION_INJECTION_BUILTIN(closelog);
  return f_closelog();
}

inline bool x_syslog(int priority, CStrRef message) {
  FUNCTION_INJECTION_BUILTIN(syslog);
  return f_syslog(priority, message);
}


///////////////////////////////////////////////////////////////////////////////
}

#endif // __EXTPROFILE_NETWORK_H__
