/*
 * Arithmetic expression lexer, recursive-descent parser, and evaluator.
 */

#include "arith.h"

#include <cctype>
#include <climits>
#include <cstdlib>
#include <type_traits>
#include <utility>

#include "shell.h"
#include "shell_context.h"

namespace dash {
namespace arith {
namespace {

enum class TokenKind {
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

struct Token {
	TokenKind kind = TokenKind::Eof;
	intmax_t num = 0;
	std::string name;
};

bool is_name_start(unsigned char c)
{
	return std::isalpha(c) || c == '_';
}

bool is_name_char(unsigned char c)
{
	return std::isalnum(c) || c == '_';
}

class Lexer {
public:
	explicit Lexer(std::string_view input) : cur_(input.data()), end_(input.data() + input.size())
	{
	}

	Token next()
	{
		while (cur_ < end_ && (*cur_ == ' ' || *cur_ == '\t' || *cur_ == '\n'))
			++cur_;

		if (cur_ >= end_ || *cur_ == '\0')
			return Token{TokenKind::Eof};

		unsigned char c = static_cast<unsigned char>(*cur_);

		if (std::isdigit(c)) {
			char *after = nullptr;
			intmax_t val = strtoimax(cur_, &after, 0);
			cur_ = after;
			return Token{TokenKind::Num, val};
		}

		if (is_name_start(c)) {
			const char *start = cur_;
			do {
				++cur_;
			} while (cur_ < end_ && is_name_char(static_cast<unsigned char>(*cur_)));
			Token tok;
			tok.kind = TokenKind::Var;
			tok.name.assign(start, cur_);
			return tok;
		}

		++cur_;
		switch (c) {
		case '(':
			return Token{TokenKind::LParen};
		case ')':
			return Token{TokenKind::RParen};
		case '?':
			return Token{TokenKind::QMark};
		case ':':
			return Token{TokenKind::Colon};
		case '~':
			return Token{TokenKind::BitNot};
		case '+':
			return Token{match('=') ? TokenKind::AddAssign : TokenKind::Add};
		case '-':
			return Token{match('=') ? TokenKind::SubAssign : TokenKind::Sub};
		case '*':
			return Token{match('=') ? TokenKind::MulAssign : TokenKind::Mul};
		case '/':
			return Token{match('=') ? TokenKind::DivAssign : TokenKind::Div};
		case '%':
			return Token{match('=') ? TokenKind::RemAssign : TokenKind::Rem};
		case '^':
			return Token{match('=') ? TokenKind::BitXorAssign : TokenKind::BitXor};
		case '!':
			return Token{match('=') ? TokenKind::Ne : TokenKind::Not};
		case '=':
			return Token{match('=') ? TokenKind::Eq : TokenKind::Assign};
		case '|':
			if (match('|'))
				return Token{TokenKind::LogOr};
			return Token{match('=') ? TokenKind::BitOrAssign : TokenKind::BitOr};
		case '&':
			if (match('&'))
				return Token{TokenKind::LogAnd};
			return Token{match('=') ? TokenKind::BitAndAssign : TokenKind::BitAnd};
		case '<':
			if (match('<'))
				return Token{match('=') ? TokenKind::LShiftAssign : TokenKind::LShift};
			return Token{match('=') ? TokenKind::Le : TokenKind::Lt};
		case '>':
			if (match('>'))
				return Token{match('=') ? TokenKind::RShiftAssign : TokenKind::RShift};
			return Token{match('=') ? TokenKind::Ge : TokenKind::Gt};
		default:
			return Token{TokenKind::Bad};
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

int binop_prec(TokenKind kind)
{
	switch (kind) {
	case TokenKind::Mul:
	case TokenKind::Div:
	case TokenKind::Rem:
		return 0;
	case TokenKind::Add:
	case TokenKind::Sub:
		return 1;
	case TokenKind::LShift:
	case TokenKind::RShift:
		return 2;
	case TokenKind::Lt:
	case TokenKind::Le:
	case TokenKind::Gt:
	case TokenKind::Ge:
		return 3;
	case TokenKind::Eq:
	case TokenKind::Ne:
		return 4;
	case TokenKind::BitAnd:
		return 5;
	case TokenKind::BitXor:
		return 6;
	case TokenKind::BitOr:
		return 7;
	default:
		return -1;
	}
}

Op token_to_binop(TokenKind kind)
{
	switch (kind) {
	case TokenKind::Add:
		return Op::Add;
	case TokenKind::Sub:
		return Op::Sub;
	case TokenKind::Mul:
		return Op::Mul;
	case TokenKind::Div:
		return Op::Div;
	case TokenKind::Rem:
		return Op::Rem;
	case TokenKind::LShift:
		return Op::LShift;
	case TokenKind::RShift:
		return Op::RShift;
	case TokenKind::Lt:
		return Op::Lt;
	case TokenKind::Le:
		return Op::Le;
	case TokenKind::Gt:
		return Op::Gt;
	case TokenKind::Ge:
		return Op::Ge;
	case TokenKind::Eq:
		return Op::Eq;
	case TokenKind::Ne:
		return Op::Ne;
	case TokenKind::BitAnd:
		return Op::BitAnd;
	case TokenKind::BitXor:
		return Op::BitXor;
	case TokenKind::BitOr:
		return Op::BitOr;
	default:
		return Op::Add;
	}
}

bool is_assign_op(TokenKind kind)
{
	switch (kind) {
	case TokenKind::Assign:
	case TokenKind::AddAssign:
	case TokenKind::SubAssign:
	case TokenKind::MulAssign:
	case TokenKind::DivAssign:
	case TokenKind::RemAssign:
	case TokenKind::LShiftAssign:
	case TokenKind::RShiftAssign:
	case TokenKind::BitAndAssign:
	case TokenKind::BitXorAssign:
	case TokenKind::BitOrAssign:
		return true;
	default:
		return false;
	}
}

Op token_to_assign(TokenKind kind)
{
	switch (kind) {
	case TokenKind::Assign:
		return Op::Assign;
	case TokenKind::AddAssign:
		return Op::AddAssign;
	case TokenKind::SubAssign:
		return Op::SubAssign;
	case TokenKind::MulAssign:
		return Op::MulAssign;
	case TokenKind::DivAssign:
		return Op::DivAssign;
	case TokenKind::RemAssign:
		return Op::RemAssign;
	case TokenKind::LShiftAssign:
		return Op::LShiftAssign;
	case TokenKind::RShiftAssign:
		return Op::RShiftAssign;
	case TokenKind::BitAndAssign:
		return Op::BitAndAssign;
	case TokenKind::BitXorAssign:
		return Op::BitXorAssign;
	case TokenKind::BitOrAssign:
		return Op::BitOrAssign;
	default:
		return Op::Assign;
	}
}

ExprPtr make_num(intmax_t value)
{
	return std::make_shared<Expr>(Expr{NumExpr{value}});
}

ExprPtr make_var(std::string name)
{
	return std::make_shared<Expr>(Expr{VarExpr{std::move(name)}});
}

ExprPtr make_unary(Op op, ExprPtr operand)
{
	return std::make_shared<Expr>(Expr{UnaryExpr{op, std::move(operand)}});
}

ExprPtr make_binary(Op op, ExprPtr lhs, ExprPtr rhs)
{
	return std::make_shared<Expr>(
		Expr{BinaryExpr{op, std::move(lhs), std::move(rhs)}});
}

ExprPtr make_cond(ExprPtr cond, ExprPtr then_expr, ExprPtr else_expr)
{
	return std::make_shared<Expr>(
		Expr{CondExpr{std::move(cond), std::move(then_expr), std::move(else_expr)}});
}

ExprPtr make_assign(Op op, std::string name, ExprPtr value)
{
	return std::make_shared<Expr>(
		Expr{AssignExpr{op, std::move(name), std::move(value)}});
}

class Parser {
public:
	explicit Parser(std::string_view input) : lexer_(input)
	{
		advance();
	}

	Result<ExprPtr> parse()
	{
		auto expr = parse_assignment();
		if (!expr)
			return expr;
		if (tok_.kind != TokenKind::Eof)
			return tl::unexpected(std::string("expecting EOF"));
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

	Result<ExprPtr> fail(const char *msg)
	{
		return tl::unexpected(std::string(msg));
	}

	Result<ExprPtr> parse_assignment()
	{
		if (tok_.kind == TokenKind::Var) {
			Token var = std::move(tok_);
			advance();
			if (is_assign_op(tok_.kind)) {
				Op op = token_to_assign(tok_.kind);
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

	Result<ExprPtr> parse_cond()
	{
		auto cond = parse_or();
		if (!cond)
			return cond;
		if (tok_.kind != TokenKind::QMark)
			return cond;

		advance();
		auto then_expr = parse_assignment();
		if (!then_expr)
			return then_expr;
		if (tok_.kind != TokenKind::Colon)
			return fail("expecting ':'");
		advance();
		auto else_expr = parse_cond();
		if (!else_expr)
			return else_expr;
		return make_cond(std::move(*cond), std::move(*then_expr), std::move(*else_expr));
	}

	Result<ExprPtr> parse_or()
	{
		auto lhs = parse_and();
		if (!lhs)
			return lhs;
		while (tok_.kind == TokenKind::LogOr) {
			advance();
			auto rhs = parse_and();
			if (!rhs)
				return rhs;
			lhs = make_binary(Op::LogOr, std::move(*lhs), std::move(*rhs));
		}
		return lhs;
	}

	Result<ExprPtr> parse_and()
	{
		auto lhs = parse_binop(7);
		if (!lhs)
			return lhs;
		while (tok_.kind == TokenKind::LogAnd) {
			advance();
			auto rhs = parse_binop(7);
			if (!rhs)
				return rhs;
			lhs = make_binary(Op::LogAnd, std::move(*lhs), std::move(*rhs));
		}
		return lhs;
	}

	/* Pratt-style binary ops for precedence levels 0..max_prec inclusive. */
	Result<ExprPtr> parse_binop(int max_prec)
	{
		auto lhs = parse_primary();
		if (!lhs)
			return lhs;

		for (;;) {
			int prec = binop_prec(tok_.kind);
			if (prec < 0 || prec > max_prec)
				return lhs;

			Op op = token_to_binop(tok_.kind);
			advance();
			auto rhs = parse_binop(prec - 1);
			if (!rhs)
				return rhs;
			lhs = make_binary(op, std::move(*lhs), std::move(*rhs));
		}
	}

	Result<ExprPtr> parse_primary()
	{
		Token cur = tok_;
		advance();

		switch (cur.kind) {
		case TokenKind::LParen: {
			auto inner = parse_assignment();
			if (!inner)
				return inner;
			if (tok_.kind != TokenKind::RParen)
				return fail("expecting ')'");
			advance();
			return inner;
		}
		case TokenKind::Num:
			return make_num(cur.num);
		case TokenKind::Var:
			return make_var(std::move(cur.name));
		case TokenKind::Add:
			return parse_primary();
		case TokenKind::Sub: {
			auto operand = parse_primary();
			if (!operand)
				return operand;
			return make_unary(Op::Neg, std::move(*operand));
		}
		case TokenKind::Not: {
			auto operand = parse_primary();
			if (!operand)
				return operand;
			return make_unary(Op::Not, std::move(*operand));
		}
		case TokenKind::BitNot: {
			auto operand = parse_primary();
			if (!operand)
				return operand;
			return make_unary(Op::BitNot, std::move(*operand));
		}
		default:
			return fail("expecting primary");
		}
	}

	Lexer lexer_;
	Token tok_;
	Token pending_;
	bool has_pending_ = false;
};

} /* namespace */

Result<ExprPtr> parse(std::string_view input)
{
	Parser parser(input);
	return parser.parse();
}

Op Evaluator::assign_to_binop(Op op)
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

Result<intmax_t> Evaluator::do_binop(Op op, intmax_t a, intmax_t b)
{
	switch (op) {
	case Op::Rem:
	case Op::Div:
		if (!b || (a == INTMAX_MIN && b == -1))
			return tl::unexpected(std::string("division error"));
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
		return tl::unexpected(std::string("bad binary operator"));
	}
}

Result<intmax_t> Evaluator::eval_node(const Expr &expr, bool noeval)
{
	return std::visit(
		[&](const auto &node) -> Result<intmax_t> {
			using T = std::decay_t<decltype(node)>;

			if constexpr (std::is_same_v<T, NumExpr>) {
				return node.value;
			} else if constexpr (std::is_same_v<T, VarExpr>) {
				if (noeval)
					return 0;
				return lookupvarint(sh_, node.name.c_str());
			} else if constexpr (std::is_same_v<T, UnaryExpr>) {
				auto v = eval_node(*node.operand, noeval);
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
					return tl::unexpected(std::string("bad unary operator"));
				}
			} else if constexpr (std::is_same_v<T, BinaryExpr>) {
				if (node.op == Op::LogAnd) {
					auto a = eval_node(*node.lhs, noeval);
					if (!a)
						return a;
					auto b = eval_node(*node.rhs, noeval || !*a);
					if (!b)
						return b;
					return *a && *b;
				}
				if (node.op == Op::LogOr) {
					auto a = eval_node(*node.lhs, noeval);
					if (!a)
						return a;
					auto b = eval_node(*node.rhs, noeval || !!*a);
					if (!b)
						return b;
					return *a || *b;
				}

				auto a = eval_node(*node.lhs, noeval);
				if (!a)
					return a;
				auto b = eval_node(*node.rhs, noeval);
				if (!b)
					return b;
				if (noeval)
					return b;
				return do_binop(node.op, *a, *b);
			} else if constexpr (std::is_same_v<T, CondExpr>) {
				auto c = eval_node(*node.cond, noeval);
				if (!c)
					return c;
				auto t = eval_node(*node.then_expr, noeval || !*c);
				if (!t)
					return t;
				auto e = eval_node(*node.else_expr, noeval || !!*c);
				if (!e)
					return e;
				return *c ? *t : *e;
			} else if constexpr (std::is_same_v<T, AssignExpr>) {
				auto rhs = eval_node(*node.value, noeval);
				if (!rhs)
					return rhs;
				if (noeval)
					return rhs;

				intmax_t result = *rhs;
				if (node.op != Op::Assign) {
					intmax_t cur = lookupvarint(sh_, node.name.c_str());
					auto combined = do_binop(assign_to_binop(node.op), cur, result);
					if (!combined)
						return combined;
					result = *combined;
				}
				return setvarint(sh_, node.name.c_str(), result, 0);
			}
		},
		expr.node);
}

Result<intmax_t> Evaluator::eval(const ExprPtr &expr)
{
	if (!expr)
		return tl::unexpected(std::string("expecting primary"));
	return eval_node(*expr, false);
}

} /* namespace arith */
} /* namespace dash */

intmax_t
arith(ShellContext &sh, const char *s)
{
	auto ast = dash::arith::parse(s ? s : "");
	if (!ast)
		sh_error(sh, "arithmetic expression: %s: \"%s\"", ast.error().c_str(), s);

	dash::arith::Evaluator evaluator(*&sh);
	auto result = evaluator.eval(*ast);
	if (!result)
		sh_error(sh, "arithmetic expression: %s: \"%s\"", result.error().c_str(), s);

	return *result;
}
