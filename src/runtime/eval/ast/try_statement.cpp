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

#include <runtime/eval/ast/try_statement.h>
#include <runtime/eval/runtime/variable_environment.h>

namespace HPHP {
namespace Eval {
///////////////////////////////////////////////////////////////////////////////

CatchBlock::CatchBlock(CONSTRUCT_ARGS, const string &ename,
                       const string &vname, StatementPtr body)
  : Construct(CONSTRUCT_PASS), m_ename(ename),
    m_vname(vname), m_sg(VariableIndex::isSuperGlobal(vname)), m_body(body) {}

bool CatchBlock::match(CObjRef exn) const {
  return exn.instanceof(m_ename.c_str());
}

bool CatchBlock::proc(CObjRef exn, VariableEnvironment &env) const {
  if (exn.instanceof(m_ename.c_str())) {
    if (m_body) {
      env.getVar(m_vname, m_sg) = exn;
      m_body->eval(env);
    }
    return true;
  }
  return false;
}

void CatchBlock::dump(std::ostream &out) const {
  out << " catch (" << m_ename << " $" << m_vname << ") {\n";
  if (m_body) m_body->dump(out);
  out << "}";
}

void optimize(VariableEnvironment &env, CatchBlockPtr &cb) {
  if (!cb) return;
  if (CatchBlockPtr optCb = cb->optimize(env)) {
    cb = optCb;
  }
}

CatchBlock *CatchBlock::optimize(VariableEnvironment &env) {
  Eval::optimize(env, m_body);
  return NULL;
}

TryStatement::TryStatement(STATEMENT_ARGS, StatementPtr body,
                           const std::vector<CatchBlockPtr> &catches)
  : Statement(STATEMENT_PASS), m_catches(catches), m_body(body) {}

Statement *TryStatement::optimize(VariableEnvironment &env) {
  for (unsigned int i = 0; i < m_catches.size(); i++) {
    Eval::optimize(env, m_catches[i]);
  }
  Eval::optimize(env, m_body);
  return NULL;
}

void TryStatement::eval(VariableEnvironment &env) const {
  //if (env.isGotoing()) return;
  ENTER_STMT;
  try {
    m_body->eval(env);
  } catch (Object e) {
    for (vector<CatchBlockPtr>::const_iterator it = m_catches.begin();
         it != m_catches.end(); ++it) {
      if ((*it)->match(e)) {
        if ((*it)->body()) {
          String s = (*it)->vname();
          SuperGlobal sg = (*it)->sg();
          env.getVar(s, sg) = e;
          EVAL_STMT((*it)->body(), env);
        }
        return;
      }
    }
    throw e;
  }
  if (env.isGotoing()) {
    for (vector<CatchBlockPtr>::const_iterator it = m_catches.begin();
         it != m_catches.end(); ++it) {
      if ((*it)->body()) {
        EVAL_STMT((*it)->body(), env);
        if (!env.isGotoing()) return;
      }
    }
  }
}

void TryStatement::dump(std::ostream &out) const {
  out << "try {\n";
  m_body->dump(out);
  out << "}";
  dumpVector(out, m_catches, "");
  out << "\n";
}

///////////////////////////////////////////////////////////////////////////////
}
}

