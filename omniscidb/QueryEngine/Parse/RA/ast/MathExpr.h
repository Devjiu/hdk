/**
 * @file	MathExpr.h
 * @author	Steven Stewart <steve@map-d.com>
 * @author	Gil Walzer <gil@map-d.com>
 */
#ifndef RA_MATHEXPR_NODE_H
#define RA_MATHEXPR_NODE_H

#include <cassert>
#include "RelAlgNode.h"
#include "../visitor/Visitor.h"

namespace RA_Namespace {

class MathExpr : public RelAlgNode {
    
public:
	MathExpr *n1 = NULL;
	MathExpr *n2 = NULL;
	Attribute *n3 = NULL;
	AggrExpr *n4 = NULL;
    bool isBinaryOp = false;
    bool isScalar = false;

    bool intFloatFlag; // true if int; otherwise float
    int intVal;
	float floatVal;
    OpType op;

	MathExpr(OpType opType, MathExpr *n1, MathExpr *n2): op(opType)  {
		assert(op == OP_ADD || op == OP_SUBTRACT || op == OP_MULTIPLY || op == OP_DIVIDE);
		assert(n1 && n2);
		this->n1 = n1;
		this->n2 = n2;
        isBinaryOp = true;
	}

	explicit MathExpr(MathExpr *n1) {
		assert(n1);
		this->n1 = n1;
	}

	explicit MathExpr(Attribute *n3) {
		assert(n3);
		this->n3 = n3;
	}

	explicit MathExpr(AggrExpr *n4) {
		assert(n4);
		this->n4 = n4;
	}

	explicit MathExpr(int intVal) {
		this->intVal = intVal;
        isScalar = true;
		intFloatFlag = true;
	}

	explicit MathExpr(float floatVal) {
		this->floatVal = floatVal;
        isScalar = true;
		intFloatFlag = false;
	}

	virtual void accept(class Visitor &v) {
		v.visit(this);
	}
};

} // RA_Namespace

#endif // RA_MATHEXPR_NODE_H
