#ifndef ARITH_H
#define ARITH_H
/*
 * Modern C++ arithmetic-expression parser for $((...)) / let-style eval.
 * Builds an AST, reports errors via tl::expected (dash::Result), and evaluates
 * with short-circuit semantics matching POSIX shell arithmetic.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "result.h"

struct ShellContext;

namespace dash {
namespace arith {

template <typename T>
using Result = dash::Result<T>;

enum class Op {
	Add,
	Sub,
	Mul,
	Div,
	Rem,
	LShift,
	RShift,
	Lt,
	Le,
	Gt,
	Ge,
	Eq,
	Ne,
	BitAnd,
	BitXor,
	BitOr,
	LogAnd,
	LogOr,
	Not,
	BitNot,
	Neg,
	Assign,
	AddAssign,
	SubAssign,
	MulAssign,
	DivAssign,
	RemAssign,
	LShiftAssign,
	RShiftAssign,
	BitAndAssign,
	BitXorAssign,
	BitOrAssign,
};

struct Expr;

using ExprPtr = std::shared_ptr<Expr>;

struct NumExpr {
	intmax_t value;
};

struct VarExpr {
	std::string name;
};

struct UnaryExpr {
	Op op;
	ExprPtr operand;
};

struct BinaryExpr {
	Op op;
	ExprPtr lhs;
	ExprPtr rhs;
};

struct CondExpr {
	ExprPtr cond;
	ExprPtr then_expr;
	ExprPtr else_expr;
};

struct AssignExpr {
	Op op; /* Assign or a compound assignment */
	std::string name;
	ExprPtr value;
};

struct Expr {
	std::variant<NumExpr, VarExpr, UnaryExpr, BinaryExpr, CondExpr, AssignExpr>
		node;
};

Result<ExprPtr> parse(std::string_view input);

class Evaluator {
public:
	explicit Evaluator(ShellContext &sh) : sh_(sh) {}

	Result<intmax_t> eval(const ExprPtr &expr);

private:
	Result<intmax_t> eval_node(const Expr &expr, bool noeval);
	Result<intmax_t> do_binop(Op op, intmax_t a, intmax_t b);
	static Op assign_to_binop(Op op);

	ShellContext &sh_;
};

} /* namespace arith */
} /* namespace dash */

#endif
