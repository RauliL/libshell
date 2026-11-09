/*
 * Arithmetic expression lexer, recursive-descent parser, and evaluator.
 */
#include <cinttypes>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "error.h"
#include "var.h"

using namespace shell;

namespace shell {

namespace {

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

struct AssignExpr;
struct BinaryExpr;
struct CondExpr;
struct NumExpr;
struct UnaryExpr;
struct VarExpr;

using Expr = std::variant<
	AssignExpr,
	BinaryExpr,
	CondExpr,
	NumExpr,
	UnaryExpr,
	VarExpr
>;
using ExprPtr = std::unique_ptr<Expr>;

struct NumExpr {
	std::intmax_t value;
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

struct Token {
	enum class Type {
		Eof,
		Bad,
		Num,
		Var,
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
		LParen,
		RParen,
		QMark,
		Colon,
	};

	Type type = Type::Eof;
	intmax_t num = 0;
	std::string name;
};

}

using EvalResult = tl::expected<std::intmax_t, std::string>;
using ParseResult = tl::expected<ExprPtr, std::string>;

namespace {
EvalResult
do_binop(Op op, std::intmax_t a, std::intmax_t b)
{
	switch (op) {
	case Op::Rem:
	case Op::Div:
		if (!b || (a == INTMAX_MIN && b == -1))
			return tl::unexpected<std::string>("division error");
		return op == Op::Rem ? a % b : a / b;
	case Op::Mul:
		return a * b;
	case Op::Add:
		return a + b;
	case Op::Sub:
		return a - b;
	case Op::LShift:
		return a << b;
	case Op::RShift:
		return a >> b;
	case Op::Lt:
		return a < b;
	case Op::Le:
		return a <= b;
	case Op::Gt:
		return a > b;
	case Op::Ge:
		return a >= b;
	case Op::Eq:
		return a == b;
	case Op::Ne:
		return a != b;
	case Op::BitAnd:
		return a & b;
	case Op::BitXor:
		return a ^ b;
	case Op::BitOr:
		return a | b;
	case Op::LogAnd:
		return a && b;
	case Op::LogOr:
		return a || b;
	default:
		return tl::unexpected<std::string>("bad binary operator");
	}
}

inline bool
is_name_start(unsigned char c)
{
	return std::isalpha(c) || c == '_';
}

inline bool
is_name_char(unsigned char c)
{
	return std::isalnum(c) || c == '_';
}
} // namespace

namespace {

class Lexer {
public:
	explicit Lexer(std::string_view input)
		: cur_(input.data())
		, end_(input.data() + input.size()) {}

	Token next()
	{
		while (cur_ < end_ && (*cur_ == ' ' || *cur_ == '\t' || *cur_ == '\n'))
			++cur_;

		if (cur_ >= end_ || *cur_ == '\0')
			return Token{Token::Type::Eof};

		unsigned char c = static_cast<unsigned char>(*cur_);

		if (std::isdigit(c)) {
			char *after = nullptr;
			auto val = std::strtoimax(cur_, &after, 0);

			cur_ = after;

			return Token{Token::Type::Num, val};
		}

		if (is_name_start(c)) {
			const char *start = cur_;
			do {
				++cur_;
			} while (cur_ < end_ && is_name_char(*cur_));
			Token tok;
			tok.type = Token::Type::Var;
			tok.name.assign(start, cur_);
			return tok;
		}

		++cur_;
		switch (c) {
			case '(':
				return Token{Token::Type::LParen};

			case ')':
				return Token{Token::Type::RParen};

			case '?':
				return Token{Token::Type::QMark};

			case ':':
				return Token{Token::Type::Colon};

			case '~':
				return Token{Token::Type::BitNot};

			case '+':
				return Token{
					match('=')
						? Token::Type::AddAssign
						: Token::Type::Add
				};

			case '-':
				return Token{
					match('=')
						? Token::Type::SubAssign
						: Token::Type::Sub
				};

			case '*':
				return Token{
					match('=')
						? Token::Type::MulAssign
						: Token::Type::Mul
				};

			case '/':
				return Token{
					match('=')
						? Token::Type::DivAssign
						: Token::Type::Div
				};

			case '%':
				return Token{
					match('=')
						? Token::Type::RemAssign
						: Token::Type::Rem
				};

			case '^':
				return Token{
					match('=')
						? Token::Type::BitXorAssign
						: Token::Type::BitXor
				};

			case '!':
				return Token{
					match('=')
						? Token::Type::Ne
						: Token::Type::Not
				};

			case '=':
				return Token{
					match('=')
						? Token::Type::Eq
						: Token::Type::Assign
				};

			case '|':
				if (match('|'))
					return Token{Token::Type::LogOr};

				return Token{
					match('=')
						? Token::Type::BitOrAssign
						: Token::Type::BitOr
				};

			case '&':
				if (match('&'))
					return Token{Token::Type::LogAnd};

				return Token{
					match('=')
						? Token::Type::BitAndAssign
						: Token::Type::BitAnd
				};

			case '<':
				if (match('<'))
					return Token{
						match('=')
							? Token::Type::LShiftAssign
							: Token::Type::LShift
					};

				return Token{match('=') ? Token::Type::Le : Token::Type::Lt};

			case '>':
				if (match('>'))
					return Token{
						match('=')
							? Token::Type::RShiftAssign
							: Token::Type::RShift
					};

				return Token{match('=') ? Token::Type::Ge : Token::Type::Gt};

			default:
				return Token{Token::Type::Bad};
		}
	}

private:
	bool match(char expected)
	{
		if (cur_ < end_ && *cur_ == expected) {
			++cur_;
			return true;
		}
		return false;
	}

	const char *cur_;
	const char *end_;
};

static inline ExprPtr
make_num(std::intmax_t value)
{
	return std::make_unique<Expr>(NumExpr{value});
}

static inline ExprPtr
make_var(std::string name)
{
	return std::make_unique<Expr>(VarExpr{std::move(name)});
}

static inline ExprPtr
make_unary(Op op, ExprPtr operand)
{
	return std::make_unique<Expr>(UnaryExpr{op, std::move(operand)});
}

static inline ExprPtr
make_binary(Op op, ExprPtr lhs, ExprPtr rhs)
{
	return std::make_unique<Expr>(BinaryExpr{
		op,
		std::move(lhs),
		std::move(rhs)
	});
}

static inline ExprPtr
make_cond(ExprPtr cond, ExprPtr then_expr, ExprPtr else_expr)
{
	return std::make_unique<Expr>(CondExpr{
		std::move(cond),
		std::move(then_expr),
		std::move(else_expr)
	});
}

static inline ExprPtr
make_assign(Op op, std::string name, ExprPtr value)
{
	return std::make_unique<Expr>(AssignExpr{
		op,
		std::move(name),
		std::move(value)
	});
}

class Parser {
public:
	explicit Parser(std::string_view input) : lexer_(input)
	{
		advance();
	}

	ParseResult parse()
	{
		auto expr = parse_assignment();
		if (!expr)
			return expr;
		if (tok_.type != Token::Type::Eof)
			return tl::unexpected<std::string>("expecting EOF");
		return expr;
	}

private:
	void advance()
	{
		if (has_pending_) {
			tok_ = std::move(pending_);
			has_pending_ = false;
			return;
		}
		tok_ = lexer_.next();
	}

	ParseResult parse_assignment()
	{
		if (tok_.type == Token::Type::Var) {
			Token var = std::move(tok_);
			advance();
			if (is_assign_op(tok_.type)) {
				Op op = token_to_assign(tok_.type);
				advance();
				auto rhs = parse_assignment();
				if (!rhs)
					return rhs;
				return make_assign(op, std::move(var.name), std::move(*rhs));
			}
			/* Not an assignment — put the variable back and parse a
			 * normal expression starting at that primary. */
			pending_ = std::move(tok_);
			has_pending_ = true;
			tok_ = std::move(var);
		}
		return parse_cond();
	}

	ParseResult parse_cond()
	{
		auto cond = parse_or();
		if (!cond)
			return cond;
		if (tok_.type != Token::Type::QMark)
			return cond;

		advance();
		auto then_expr = parse_assignment();
		if (!then_expr)
			return then_expr;
		if (tok_.type != Token::Type::Colon)
			return tl::unexpected<std::string>("expecting ':'");
		advance();
		auto else_expr = parse_cond();
		if (!else_expr)
			return else_expr;
		return make_cond(std::move(*cond), std::move(*then_expr), std::move(*else_expr));
	}

	ParseResult parse_or()
	{
		auto lhs = parse_and();
		if (!lhs)
			return lhs;
		while (tok_.type == Token::Type::LogOr) {
			advance();
			auto rhs = parse_and();
			if (!rhs)
				return rhs;
			lhs = make_binary(Op::LogOr, std::move(*lhs), std::move(*rhs));
		}
		return lhs;
	}

	ParseResult parse_and()
	{
		auto lhs = parse_binop(7);
		if (!lhs)
			return lhs;
		while (tok_.type == Token::Type::LogAnd) {
			advance();
			auto rhs = parse_binop(7);
			if (!rhs)
				return rhs;
			lhs = make_binary(Op::LogAnd, std::move(*lhs), std::move(*rhs));
		}
		return lhs;
	}

	// Pratt-style binary ops for precedence levels 0..max_prec inclusive.
	ParseResult parse_binop(int max_prec)
	{
		auto lhs = parse_primary();
		if (!lhs)
			return lhs;

		for (;;) {
			int prec = binop_prec(tok_.type);
			if (prec < 0 || prec > max_prec)
				return lhs;

			Op op = token_to_binop(tok_.type);
			advance();
			auto rhs = parse_binop(prec - 1);
			if (!rhs)
				return rhs;
			lhs = make_binary(op, std::move(*lhs), std::move(*rhs));
		}
	}

	ParseResult parse_primary()
	{
		Token cur = tok_;

		advance();

		switch (cur.type) {
			case Token::Type::LParen: {
				auto inner = parse_assignment();
				if (!inner)
					return inner;
				if (tok_.type != Token::Type::RParen)
					return tl::unexpected<std::string>("expecting ')'");
				advance();
				return inner;
			}

			case Token::Type::Num:
				return make_num(cur.num);

			case Token::Type::Var:
				return make_var(std::move(cur.name));

			case Token::Type::Add:
				return parse_primary();

			case Token::Type::Sub: {
				auto operand = parse_primary();
				if (!operand)
					return operand;
				return make_unary(Op::Neg, std::move(*operand));
			}

			case Token::Type::Not: {
				auto operand = parse_primary();
				if (!operand)
					return operand;
				return make_unary(Op::Not, std::move(*operand));
			}

			case Token::Type::BitNot: {
				auto operand = parse_primary();
				if (!operand)
					return operand;
				return make_unary(Op::BitNot, std::move(*operand));
			}

			default:
				return tl::unexpected<std::string>("expecting primary");
		}
	}

	static int binop_prec(Token::Type kind)
	{
		switch (kind) {
			case Token::Type::Mul:
			case Token::Type::Div:
			case Token::Type::Rem:
				return 0;
			case Token::Type::Add:
			case Token::Type::Sub:
				return 1;
			case Token::Type::LShift:
			case Token::Type::RShift:
				return 2;
			case Token::Type::Lt:
			case Token::Type::Le:
			case Token::Type::Gt:
			case Token::Type::Ge:
				return 3;
			case Token::Type::Eq:
			case Token::Type::Ne:
				return 4;
			case Token::Type::BitAnd:
				return 5;
			case Token::Type::BitXor:
				return 6;
			case Token::Type::BitOr:
				return 7;
			default:
				return -1;
		}
	}

	static bool is_assign_op(Token::Type kind)
	{
		switch (kind) {
			case Token::Type::Assign:
			case Token::Type::AddAssign:
			case Token::Type::SubAssign:
			case Token::Type::MulAssign:
			case Token::Type::DivAssign:
			case Token::Type::RemAssign:
			case Token::Type::LShiftAssign:
			case Token::Type::RShiftAssign:
			case Token::Type::BitAndAssign:
			case Token::Type::BitXorAssign:
			case Token::Type::BitOrAssign:
				return true;

			default:
				return false;
		}
	}

	static Op token_to_assign(Token::Type kind)
	{
		switch (kind) {
			case Token::Type::Assign:
				return Op::Assign;
			case Token::Type::AddAssign:
				return Op::AddAssign;
			case Token::Type::SubAssign:
				return Op::SubAssign;
			case Token::Type::MulAssign:
				return Op::MulAssign;
			case Token::Type::DivAssign:
				return Op::DivAssign;
			case Token::Type::RemAssign:
				return Op::RemAssign;
			case Token::Type::LShiftAssign:
				return Op::LShiftAssign;
			case Token::Type::RShiftAssign:
				return Op::RShiftAssign;
			case Token::Type::BitAndAssign:
				return Op::BitAndAssign;
			case Token::Type::BitXorAssign:
				return Op::BitXorAssign;
			case Token::Type::BitOrAssign:
				return Op::BitOrAssign;
			default:
				return Op::Assign;
		}
	}

	static Op token_to_binop(Token::Type kind)
	{
		switch (kind) {
			case Token::Type::Add:
				return Op::Add;
			case Token::Type::Sub:
				return Op::Sub;
			case Token::Type::Mul:
				return Op::Mul;
			case Token::Type::Div:
				return Op::Div;
			case Token::Type::Rem:
				return Op::Rem;
			case Token::Type::LShift:
				return Op::LShift;
			case Token::Type::RShift:
				return Op::RShift;
			case Token::Type::Lt:
				return Op::Lt;
			case Token::Type::Le:
				return Op::Le;
			case Token::Type::Gt:
				return Op::Gt;
			case Token::Type::Ge:
				return Op::Ge;
			case Token::Type::Eq:
				return Op::Eq;
			case Token::Type::Ne:
				return Op::Ne;
			case Token::Type::BitAnd:
				return Op::BitAnd;
			case Token::Type::BitXor:
				return Op::BitXor;
			case Token::Type::BitOr:
				return Op::BitOr;
			default:
				return Op::Add;
		}
	}

	Lexer lexer_;
	Token tok_;
	Token pending_;
	bool has_pending_ = false;
};

} /* namespace */

namespace {
Op
assign_to_binop(Op op)
{
	switch (op) {
		case Op::AddAssign:
			return Op::Add;
		case Op::SubAssign:
			return Op::Sub;
		case Op::MulAssign:
			return Op::Mul;
		case Op::DivAssign:
			return Op::Div;
		case Op::RemAssign:
			return Op::Rem;
		case Op::LShiftAssign:
			return Op::LShift;
		case Op::RShiftAssign:
			return Op::RShift;
		case Op::BitAndAssign:
			return Op::BitAnd;
		case Op::BitXorAssign:
			return Op::BitXor;
		case Op::BitOrAssign:
			return Op::BitOr;
		default:
			return Op::Add;
	}
}
} // namespace

namespace {

template<class... Ts>
struct overloaded : Ts... {
	using Ts::operator()...;
};

}

namespace {
EvalResult
eval(Context &context, const Expr &expr, bool noeval)
{
	return std::visit(
		overloaded{
			[](const NumExpr &node) -> EvalResult {
				return node.value;
			},

			[&](const VarExpr &node) -> EvalResult {
				if (noeval)
					return 0;

				const auto v = context.get_int_var(node.name);

				if (!v)
					return tl::unexpected<std::string>("variable lookup failed");

				return *v;
			},

			[&](const UnaryExpr &node) -> EvalResult {
				const auto v = eval(context, *node.operand, noeval);

				if (!v || noeval)
					return v;

				switch (node.op) {
					case Op::Neg:
						return -*v;
					case Op::Not:
						return !*v;
					case Op::BitNot:
						return ~*v;
					default:
						return tl::unexpected<std::string>("bad unary operator");
				}
			},

			[&](const BinaryExpr &node) -> EvalResult {
				if (node.op == Op::LogAnd) {
					const auto a = eval(context, *node.lhs, noeval);
					if (!a)
						return a;

					const auto b = eval(context, *node.rhs, noeval || !*a);
					if (!b)
						return b;

					return *a && *b;
				}
				if (node.op == Op::LogOr) {
					const auto a = eval(context, *node.lhs, noeval);
					if (!a)
						return a;

					const auto b = eval(context, *node.rhs, noeval || !!*a);
					if (!b)
						return b;

					return *a || *b;
				}

				const auto a = eval(context, *node.lhs, noeval);
				if (!a)
					return a;

				const auto b = eval(context, *node.rhs, noeval);
				if (!b || noeval)
					return b;

				return do_binop(node.op, *a, *b);
			},

			[&](const CondExpr &node) -> EvalResult {
				const auto c = eval(context, *node.cond, noeval);
				if (!c)
					return c;

				const auto t = eval(context, *node.then_expr, noeval || !*c);
				if (!t)
					return t;

				const auto e = eval(context, *node.else_expr, noeval || !!*c);
				if (!e)
					return e;

				return *c ? *t : *e;
			},

			[&](const AssignExpr &node) -> EvalResult {
				const auto rhs = eval(context, *node.value, noeval);
				if (!rhs || noeval)
					return rhs;

				std::intmax_t result = *rhs;

				if (node.op != Op::Assign) {
					const auto cur = context.get_int_var(node.name);
					if (!cur)
						return tl::unexpected<std::string>("variable lookup failed");

					const auto combined = do_binop(assign_to_binop(node.op), *cur, result);
					if (!combined)
						return combined;

					result = *combined;
				}

				if (const auto r = context.set_int_var(node.name, result); !r)
					return tl::unexpected<std::string>("variable assignment failed");

				return result;
			},
		},
		expr
	);
}

EvalResult
eval(Context &context, const ExprPtr &expr)
{
	if (!expr)
		return tl::unexpected<std::string>("expecting primary");

	return eval(context, *expr, false);
}
} // namespace

Result<std::intmax_t>
Context::arith(std::string_view expr)
{
	const auto ast = Parser(expr).parse();
	if (!ast)
		return sh_error(
			*this,
			"arithmetic expression: {}: \"{}\"",
			ast.error(),
			expr
		);

	const auto result = ::eval(*this, *ast);
	if (!result)
		return sh_error(
			*this,
			"arithmetic expression: {}: \"{}\"",
			result.error(),
			expr
		);

	return *result;
}

} // namespace shell
