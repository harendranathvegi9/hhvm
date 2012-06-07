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

#include <compiler/expression/static_class_name.h>
#include <compiler/expression/scalar_expression.h>
#include <compiler/statement/statement_list.h>
#include <compiler/analysis/class_scope.h>
#include <compiler/analysis/file_scope.h>
#include <util/util.h>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

StaticClassName::StaticClassName(ExpressionPtr classExp)
    : m_class(classExp),
      m_self(false), m_parent(false), m_static(false),
      m_redeclared(false), m_present(false), m_forcePresent(false),
      m_unknown(true) {
  updateClassName();
  if (m_origClassName == "parent") {
    m_parent = true;
  } else if (m_origClassName == "self") {
    m_self = true;
  } else if (m_origClassName == "static") {
    m_static = true;
    m_present = true;
    m_class = classExp;
    m_className = m_origClassName = "";
  }
}

void StaticClassName::updateClassName() {
  if (m_class && m_class->is(Expression::KindOfScalarExpression) &&
      !m_static) {
    ScalarExpressionPtr s(dynamic_pointer_cast<ScalarExpression>(m_class));
    const string &className = s->getString();
    m_className = Util::toLower(className);
    m_origClassName = className;
    m_class.reset();
  } else {
    m_className = "";
  }
}

void StaticClassName::resolveStatic(const string &name) {
  assert(isStatic());
  m_static = m_self = m_parent = false;
  m_present = false;
  m_class.reset();
  m_origClassName = name;
  m_className = Util::toLower(name);
}

ClassScopePtr StaticClassName::resolveClass() {
  m_present = false;
  m_unknown = true;
  if (m_class) return ClassScopePtr();
  BlockScopeRawPtr scope = dynamic_cast<Expression*>(this)->getOriginalScope();
  if (m_self) {
    if (ClassScopePtr self = scope->getContainingClass()) {
      m_className = self->getName();
      m_origClassName = self->getOriginalName();
      m_present = true;
      m_unknown = false;
      return self;
    }
  } else if (m_parent) {
    if (ClassScopePtr self = scope->getContainingClass()) {
      if (!self->getOriginalParent().empty()) {
        m_className = Util::toLower(self->getOriginalParent());
        m_origClassName = self->getOriginalParent();
        m_present = true;
      }
    } else if (!hhvm) {
      // When generating hhvm bytecodes, the following statement
      // causes EmitterVisitor::emitFuncCall to not generate a Parent
      // byte code. It's unclear whether removing it would break hphpc
      // so this code is left here under a hhvm flag check for now.
      m_parent = false;
    }
  }
  ClassScopePtr cls = scope->getContainingProgram()->findClass(m_className);
  if (cls) {
    m_unknown = false;
    if (cls->isVolatile()) {
      ClassScopeRawPtr c = scope->getContainingFile()->resolveClass(cls);
      if (!c) {
        c = scope->getContainingClass();
        if (c && c->getName() != m_className) c.reset();
      }
      m_present = c.get() != 0;
      if (cls->isRedeclaring()) {
        cls = c;
        if (!m_present) m_redeclared = true;
      }
    } else {
      m_present = true;
    }
  }
  return cls;
}

bool StaticClassName::checkPresent() {
  if (m_self || m_parent || m_static) return true;
  BlockScopeRawPtr scope = dynamic_cast<Expression*>(this)->getOriginalScope();
  FileScopeRawPtr currentFile = scope->getContainingFile();
  if (currentFile) {
    AnalysisResultPtr ar = currentFile->getContainingProgram();
    ClassScopeRawPtr cls = ar->findClass(m_className);
    if (!cls) return false;
    if (!cls->isVolatile()) return true;
    if (currentFile->resolveClass(cls)) return true;
    if (currentFile->checkClass(m_className)) return true;
  }

  if (ClassScopePtr self = scope->getContainingClass()) {
    if (m_className == self->getName() ||
        self->derivesFrom(scope->getContainingProgram(), m_className,
                          true, false)) {
      return true;
    }
  }

  return false;
}

///////////////////////////////////////////////////////////////////////////////

void StaticClassName::outputPHP(CodeGenerator &cg, AnalysisResultPtr ar) {
  if (m_class) {
    m_class->outputPHP(cg, ar);
  } else {
    cg_printf("%s", m_origClassName.c_str());
  }
}

///////////////////////////////////////////////////////////////////////////////
}
