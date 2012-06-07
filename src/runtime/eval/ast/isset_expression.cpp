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

#include <runtime/eval/ast/isset_expression.h>
#include <util/parser/hphp.tab.hpp>

namespace HPHP {
namespace Eval {

///////////////////////////////////////////////////////////////////////////////

IssetExpression::IssetExpression(EXPRESSION_ARGS,
                                 const std::vector<ExpressionPtr> &exps)
  : Expression(KindOfIssetExpression, EXPRESSION_PASS), m_exps(exps) {}

Expression *IssetExpression::optimize(VariableEnvironment &env) {
  for (unsigned int i = 0; i < m_exps.size(); i++) {
    Eval::optimize(env, m_exps[i]);
  }
  return NULL;
}

Variant IssetExpression::eval(VariableEnvironment &env) const {
  for (vector<ExpressionPtr>::const_iterator it = m_exps.begin();
       it != m_exps.end(); ++it) {
    if (!(*it)->exist(env, T_ISSET)) return false;
  }
  return true;
}

void IssetExpression::dump(std::ostream &out) const {
  out << "isset(";
  dumpVector(out, m_exps);
  out << ")";
}

///////////////////////////////////////////////////////////////////////////////
}
}

