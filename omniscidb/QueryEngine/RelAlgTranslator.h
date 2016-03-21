#ifndef QUERYENGINE_RELALGTRANSLATOR_H
#define QUERYENGINE_RELALGTRANSLATOR_H

#include "RelAlgAbstractInterpreter.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace Analyzer {

class Expr;

}  // namespace Analyzer

namespace Catalog_Namespace {

class Catalog;

}  // namespace Catalog_Namespace

class RelAlgTranslator {
 public:
  RelAlgTranslator(const Catalog_Namespace::Catalog& cat,
                   const std::unordered_map<const RelAlgNode*, int>& input_to_nest_level)
      : cat_(cat), input_to_nest_level_(input_to_nest_level) {}

  std::shared_ptr<Analyzer::Expr> translateScalarRex(const RexScalar* rex) const;

  static std::shared_ptr<Analyzer::Expr> translateAggregateRex(
      const RexAgg* rex,
      const std::vector<std::shared_ptr<Analyzer::Expr>>& scalar_sources);

 private:
  static std::shared_ptr<Analyzer::Expr> translateLiteral(const RexLiteral*);

  std::shared_ptr<Analyzer::Expr> translateInput(const RexInput*) const;

  std::shared_ptr<Analyzer::Expr> translateUoper(const RexOperator*) const;

  std::shared_ptr<Analyzer::Expr> translateOper(const RexOperator*) const;

  std::shared_ptr<Analyzer::Expr> translateCase(const RexCase*) const;

  std::shared_ptr<Analyzer::Expr> translateLike(const RexFunctionOperator* rex_function) const;

  std::shared_ptr<Analyzer::Expr> translateExtract(const RexFunctionOperator*) const;

  std::shared_ptr<Analyzer::Expr> translateLength(const RexFunctionOperator*) const;

  std::shared_ptr<Analyzer::Expr> translateItem(const RexFunctionOperator*) const;

  std::shared_ptr<Analyzer::Expr> translateFunction(const RexFunctionOperator*) const;

  const Catalog_Namespace::Catalog& cat_;
  const std::unordered_map<const RelAlgNode*, int> input_to_nest_level_;
};

#endif  // QUERYENGINE_RELALGTRANSLATOR_H
