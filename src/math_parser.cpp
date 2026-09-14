/* Copyright (C) 2011 Rainmeter Project Developers
 *
 * This Source Code Form is subject to the terms of the GNU General Public
 * License; either version 2 of the License, or (at your option) any later
 * version. If a copy of the GPL was not distributed with this file, You can
 * obtain one at <https://www.gnu.org/licenses/gpl-2.0.html>. */

// Adapted from Rainmeter 4.5.26 Common/MathParser at commit
// 5a124b6a09e2f7f67f8be9232718c489100e6173 for portable C++17 types.

// Heavily based on ccalc 0.5.1 by Walery Studennikov <hqsoftware@mail.ru>

#include "math_parser.h"

#include <climits>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <string>

namespace MathParser {

static const double kE = 2.7182818284590452354;
static const double kPi = 3.14159265358979323846;

typedef double (*SingleArgFunction)(double arg);
typedef const wchar_t* (*MultiArgFunction)(int paramcnt, double* args, double* result);

enum class Operator : uint8_t
{
	ShiftLeft,
	ShiftRight,
	Power,
	NotEqual,
	GreatorOrEqual,
	LessOrEqual,
	LogicalAND,
	LogicalOR,
	OpeningBracket,
	Addition,
	Subtraction,
	Multiplication,
	Division,
	Modulo,
	UNK,
	BitwiseXOR,
	BitwiseNOT,
	BitwiseAND,
	BitwiseOR,
	Equal,
	Greater,
	Less,
	Conditional,
	ConditionalSeparator,
	ClosingBracket,
	Comma,
	SingleArgFunction,
	MultiArgFunction,
	Invalid // Must be last.
};

enum class CharType
{
	Unknown     = 0x00,
	Letter      = 0x01,
	Digit       = 0x02,
	Separator   = 0x04,
	Symbol      = 0x08,
	MinusSymbol = 0x10,
	Final       = 0x7F
};

enum class Token
{
	Error,
	None,
	Final,
	Operator,
	Number,
	Name
};

struct Operation
{
	Operator type;
	uint8_t funcIndex;
	int prevTop;
};

struct Function
{
	const wchar_t* name;
	SingleArgFunction single;
	MultiArgFunction multi;
	uint8_t length;
};

static double frac(double x);
static double rad(double deg);
static double deg(double rad);
static double sgn(double x);
static double neg(double x);
static const wchar_t* Min(int paramcnt, double* args, double* result);
static const wchar_t* Max(int paramcnt, double* args, double* result);
static const wchar_t* Clamp(int paramcnt, double* args, double* result);
static const wchar_t* round(int paramcnt, double* args, double* result);
static const wchar_t* ATan2(int paramcnt, double* args, double* result);

enum {
	FUNC_ATAN2,			// note: must be before atan so it gets matched first!
	FUNC_ATAN,
	FUNC_COS,
	FUNC_SIN,
	FUNC_TAN,
	FUNC_ABS,
	FUNC_EXP,
	FUNC_LN,
	FUNC_LOG,
	FUNC_SQRT,
	FUNC_FRAC,
	FUNC_TRUNC,
	FUNC_FLOOR,
	FUNC_CEIL,
	FUNC_ROUND,
	FUNC_ASIN,
	FUNC_ACOS,
	FUNC_RAD,
	FUNC_DEG,
	FUNC_SGN,
	FUNC_NEG,
	FUNC_MIN,
	FUNC_MAX,
	FUNC_CLAMP,
	FUNC_E,
	FUNC_PI,
	// ... //
	NUM_FUNCS
};

static Function g_Functions[NUM_FUNCS] =
{
	{ L"atan2", nullptr, &ATan2, 5 },
	{ L"atan", &atan, nullptr, 4 },
	{ L"cos", &cos, nullptr, 3 },
	{ L"sin", &sin, nullptr, 3 },
	{ L"tan", &tan, nullptr, 3 },
	{ L"abs", &fabs, nullptr, 3 },
	{ L"exp", &exp, nullptr, 3 },
	{ L"ln", &log, nullptr, 2 },
	{ L"log", &log10, nullptr, 3 },
	{ L"sqrt", &sqrt, nullptr, 4 },
	{ L"frac", &frac, nullptr, 4 },
	{ L"trunc", &trunc, nullptr, 5 },
	{ L"floor", &floor, nullptr, 5 },
	{ L"ceil", &ceil, nullptr, 4 },
	{ L"round", nullptr, &round, 5 },
	{ L"asin", &asin, nullptr, 4 },
	{ L"acos", &acos, nullptr, 4 },
	{ L"rad", &rad, nullptr, 3 },
	{ L"deg", &deg, nullptr, 3 },
	{ L"sgn", &sgn, nullptr, 3 },
	{ L"neg", &neg, nullptr, 3 },
	{ L"min", nullptr, &Min, 3 },
	{ L"max", nullptr, &Max, 3 },
	{ L"clamp", nullptr, &Clamp, 5 },
	{ L"e", nullptr, nullptr, 1 },
	{ L"pi", nullptr, nullptr, 2 }
};

static const int FUNC_MAX_LEN = 5;
static const uint8_t FUNC_INVALID = UCHAR_MAX;

static const Operation g_BrOp = { Operator::OpeningBracket, 0, 0};
static const Operation g_NegOp = { Operator::SingleArgFunction, FUNC_NEG, 0 };

static const uint8_t g_OpPriorities[(uint8_t)Operator::Invalid] =
{
	5, // Operator::ShiftLeft
	5, // Operator::ShiftRight
	5, // Operator::Power
	2, // Operator::NotEqual
	2, // Operator::GreatorOrEqual
	2, // Operator::LessOrEqual
	2, // Operator::LogicalAND
	2, // Operator::LogicalOR
	0, // Operator::OpeningBracket
	3, // Operator::Addition
	3, // Operator::Subtraction
	4, // Operator::Multiplication
	4, // Operator::Division
	4, // Operator::Modulo
	4, // Operator::UNK
	5, // Operator::BitwiseXOR
	5, // Operator::BitwiseNOT
	5, // Operator::BitwiseAND
	5, // Operator::BitwiseOR
	2, // Operator::Equal
	2, // Operator::Greater
	2, // Operator::Less
	1, // Operator::Conditional
	2, // Operator::ConditionalSeparator
	0, // Operator::ClosingBracket
	2, // Operator::Comma
	6, // Operator::SingleArgFunction
	6  // Operator::MultiArgFunction
};

static CharType GetCharType(wchar_t ch);
static uint8_t GetFunctionIndex(const wchar_t* str, uint8_t len);
static Operator GetOperator(const wchar_t* str);

struct Parser
{
	Operation opStack[96];
	double numStack[64];
	int opTop;
	int valTop;
	int obrDist;

	Parser() : opStack(), numStack(), opTop(0), valTop(-1), obrDist(2) { opStack[0].type = Operator::OpeningBracket; }
};

static const wchar_t* CalcToObr(Parser& parser);
static const wchar_t* Calc(Parser& parser);

struct Lexer
{
	const wchar_t* string;
	const wchar_t* name;
	size_t nameLen;

	Token token;
	union
	{
		Operator oper;  // token == Token::Operator
		double num;  // token == Token::Number
	} value;

	CharType charType;

	Lexer(const wchar_t* str) : string(str), name(), nameLen(), token(Token::None), value(), charType(GetCharType(*str)) {}
};

static Token GetNextToken(Lexer& lexer);

const wchar_t* eBrackets = L"Unmatched brackets";
const wchar_t* eSyntax = L"Syntax error";
const wchar_t* eInternal = L"Internal error";
const wchar_t* eExtraOp = L"Extra operation";
const wchar_t* eInfinity = L"Division by 0";
const wchar_t* eUnknFunc = L"\"%s\" is unknown";
const wchar_t* eLogicErr = L"Logical expression error";
const wchar_t* eInvPrmCnt = L"Invalid function parameter count";

const wchar_t* Check(const wchar_t* formula)
{
	int brackets = 0;

	// Brackets Matching
	while (*formula)
	{
		if (*formula == L'(')
		{
			++brackets;
		}
		else if (*formula == L')')
		{
			--brackets;
		}
		++formula;
	}

	return (brackets != 0) ? eBrackets : nullptr;
}

const wchar_t* CheckedParse(const wchar_t* formula, double* result)
{
	const wchar_t* error = Check(formula);
	if (!error)
	{
		error = Parse(formula, result);
	}
	return error;
}

const wchar_t* Parse(
	const wchar_t* formula, double* result, GetValueFunc getValue, void* getValueContext)
{
	static wchar_t errorBuffer[128];

	if (!*formula)
	{
		*result = 0.0;
		return nullptr;
	}

	Parser parser;
	Lexer lexer(formula);

	const wchar_t* error;
	for (;;)
	{
		if ((parser.opTop == std::size(parser.opStack) - 2) ||
			(parser.valTop == std::size(parser.numStack) - 2))
		{
			return eInternal;
		}

		Token token = GetNextToken(lexer);
		--parser.obrDist;
		switch (token)
		{
		case Token::Error:
			return eSyntax;

		case Token::Final:
			if ((error = CalcToObr(parser)) != nullptr)
			{
				return error;
			}
			else if (parser.opTop != -1 || parser.valTop != 0)
			{
				return eInternal;
			}
			else
			{
				// Done!
				*result = parser.numStack[0];
				return nullptr;
			}
			break;

		case Token::Number:
			parser.numStack[++parser.valTop] = lexer.value.num;
			break;

		case Token::Operator:
			switch (lexer.value.oper)
			{
			case Operator::OpeningBracket:
				{
					parser.opStack[++parser.opTop] = g_BrOp;
					parser.obrDist = 2;
				}
				break;

			case Operator::ClosingBracket:
				{
					if ((error = CalcToObr(parser)) != nullptr) return error;
				}
				break;

			case Operator::Comma:
				{
					if ((error = CalcToObr(parser)) != nullptr) return error;

					if (parser.opStack[parser.opTop].type == Operator::MultiArgFunction)
					{
						parser.opStack[++parser.opTop] = g_BrOp;
						parser.obrDist = 2;
					}
					else
					{
						return eSyntax;
					}
				}
				break;

			default:
				{
					Operation op = {};
					op.type = lexer.value.oper;
					switch (op.type)
					{
					case Operator::Addition:
						if (parser.obrDist >= 1)
						{
							// Goto next token
							continue;
						}
						break;

					case Operator::Subtraction:
						if (parser.obrDist >= 1)
						{
							parser.opStack[++parser.opTop] = g_NegOp;

							// Goto next token
							continue;
						}
						break;

					case Operator::Conditional:
					case Operator::ConditionalSeparator:
						parser.obrDist = 2;
						break;

					default:
						break;
					}

					while (g_OpPriorities[(int)op.type] <= g_OpPriorities[(int)parser.opStack[parser.opTop].type])
					{
						if ((error = Calc(parser)) != nullptr) return error;
					}
					parser.opStack[++parser.opTop] = op;
				}
				break;
			}
			break;

		case Token::Name:
			{
				Operation op = {};
				if (lexer.nameLen <= FUNC_MAX_LEN &&
					((op.funcIndex = GetFunctionIndex(lexer.name, (uint8_t)lexer.nameLen)) != FUNC_INVALID))
				{
					switch (op.funcIndex)
					{
					case FUNC_E:
						parser.numStack[++parser.valTop] = kE;
						break;

					case FUNC_PI:
						parser.numStack[++parser.valTop] = kPi;
						break;

					case FUNC_ATAN2:
					case FUNC_ROUND:
					case FUNC_MIN:
					case FUNC_MAX:
					case FUNC_CLAMP:
						op.type = Operator::MultiArgFunction;
						op.prevTop = parser.valTop;
						parser.opStack[++parser.opTop] = op;
						break;

					default:	// Internal function
						op.type = Operator::SingleArgFunction;
						parser.opStack[++parser.opTop] = op;
						break;
					}
				}
				else
				{
					double dblval;
					if (getValue && getValue(lexer.name, (int)lexer.nameLen, &dblval, getValueContext))
					{
						parser.numStack[++parser.valTop] = dblval;
						break;
					}

					const std::wstring name(lexer.name, lexer.nameLen);
					std::swprintf(errorBuffer, std::size(errorBuffer), eUnknFunc, name.c_str());
					return errorBuffer;
				}
				break;
			}

		default:
			return eSyntax;
		}
	}
}

static const wchar_t* Calc(Parser& parser)
{
	double res;
	Operation op = parser.opStack[parser.opTop--];

	// Multi-argument function
	if (op.type == Operator::Conditional)
	{
		return nullptr;
	}
	else if (op.type == Operator::MultiArgFunction)
	{
		int paramcnt = parser.valTop - op.prevTop;

		parser.valTop = op.prevTop;
		const wchar_t* error = g_Functions[op.funcIndex].multi(paramcnt, &parser.numStack[parser.valTop + 1], &res);
		if (error) return error;

		parser.numStack[++parser.valTop] = res;
		return nullptr;
	}
	else if (parser.valTop < 0)
	{
		return eExtraOp;
	}

	// Right arg
	double right = parser.numStack[parser.valTop--];

	// One arg operations
	if (op.type == Operator::BitwiseNOT)
	{
		res = (double)(~((long long)right));
	}
	else if (op.type == Operator::SingleArgFunction)
	{
		res = g_Functions[op.funcIndex].single(right);
	}
	else
	{
		if (parser.valTop < 0)
		{
			return eExtraOp;
		}

		// Left arg
		double left = parser.numStack[parser.valTop--];
		switch (op.type)
		{
		case Operator::ShiftLeft:
			res = (double)((long long)left << (long long)right);
			break;

		case Operator::ShiftRight:
			res = (double)((long long)left >> (long long)right);
			break;

		case Operator::Power:
			res = pow(left, right);
			break;

		case Operator::NotEqual:
			res = left != right;
			break;

		case Operator::GreatorOrEqual:
			res = left >= right;
			break;

		case Operator::LessOrEqual:
			res = left <= right;
			break;

		case Operator::LogicalAND:
			res = left && right;
			break;

		case Operator::LogicalOR:
			res = left || right;
			break;

		case Operator::Addition:
			res = left + right;
			break;

		case Operator::Subtraction:
			res = left - right;
			break;

		case Operator::Multiplication:
			res = left*  right;
			break;

		case Operator::Division:
			if (right == 0.0)
			{
				return eInfinity;
			}
			else
			{
				res = left / right;
			}
			break;

		case Operator::Modulo:
			res = fmod(left, right);
			break;

		case Operator::UNK:
			if (left <= 0)
			{
				res = 0.0;
			}
			else if (right == 0.0)
			{
				return eInfinity;
			}
			else
			{
				res = ceil(left / right);
			}
			break;

		case Operator::BitwiseXOR:
			res = (double)((long long)left ^ (long long)right);
			break;

		case Operator::BitwiseAND:
			res = (double)((long long)left & (long long)right);
			break;

		case Operator::BitwiseOR:
			res = (double)((long long)left | (long long)right);
			break;

		case Operator::Equal:
			res = left == right;
			break;

		case Operator::Greater:
			res = left > right;
			break;

		case Operator::Less:
			res = left < right;
			break;

		case Operator::ConditionalSeparator:
			{
				// Needs three arguments
				if (parser.opTop < 0 || parser.opStack[parser.opTop--].type != Operator::Conditional)
				{
					return eLogicErr;
				}
				res = parser.numStack[parser.valTop--] ? left : right;
			}
			break;

		default:
			return eInternal;
		}
	}

	parser.numStack[++parser.valTop] = res;
	return nullptr;
}

static const wchar_t* CalcToObr(Parser& parser)
{
	while (parser.opStack[parser.opTop].type != Operator::OpeningBracket)
	{
		const wchar_t* error = Calc(parser);
		if (error) return error;
	}
	--parser.opTop;
	return nullptr;
}

Token GetNextToken(Lexer& lexer)
{
	while (lexer.charType == CharType::Separator)
	{
		lexer.charType = GetCharType(*++lexer.string);
	}

	if (lexer.charType == CharType::MinusSymbol)
	{
		// If the - sign follows a symbol, it is treated as a (negative) number.
		lexer.charType = CharType::Symbol;
		if (lexer.token == Token::Operator &&
			lexer.value.oper != Operator::OpeningBracket &&  // Special case for e.g. (-PI/2), (-(5)-2).
			lexer.value.oper != Operator::ClosingBracket)  // Special case for e.g. (5)-2.
		{
			lexer.charType = CharType::Digit;
		}
	}

	switch (lexer.charType)
	{
	case CharType::Final:
		{
			lexer.token = Token::Final;
		}
		break;

	case CharType::Letter:
		{
			lexer.token = Token::Name;
			lexer.name = lexer.string;
			do
			{
				lexer.charType = GetCharType(*++lexer.string);
			}
			while (lexer.charType <= CharType::Digit);
			lexer.nameLen = lexer.string - lexer.name;
		}
		break;

	case CharType::Digit:
		{
			wchar_t* newString;
			if (lexer.string[0] == L'0')
			{
				bool valid = true;
				long long num = 0;
				switch (lexer.string[1])
				{
				case L'x':	// Hexadecimal
					num = wcstoll(lexer.string, &newString, 16);
					break;

				case L'o':	// Octal
					num = wcstoll(lexer.string + 2, &newString, 8);
					break;

				case L'b':	// Binary
					num = wcstoll(lexer.string + 2, &newString, 2);
					break;

				default:
					valid = false;
					break;
				}

				if (valid)
				{
					if (lexer.string != newString)
					{
						lexer.token = Token::Number;
						lexer.value.num = (double)num;
						lexer.string = newString;
						lexer.charType = GetCharType(*lexer.string);
					}
					break;
				}
			}

			// Decimal
			double num = wcstod(lexer.string, &newString);
			if (lexer.string != newString)
			{
				lexer.token = Token::Number;
				lexer.value.num = num;
				lexer.string = newString;
				lexer.charType = GetCharType(*lexer.string);
			}
		}
		break;

	case CharType::Symbol:
		{
			Operator oper = GetOperator(lexer.string);
			if (oper != Operator::Invalid)
			{
				lexer.token = Token::Operator;
				lexer.value.oper = oper;
				lexer.string += ((int)oper <= (int)Operator::LogicalOR) ? 2 : 1;
				lexer.charType = GetCharType(*lexer.string);
			}
		}
		break;

	default:
		lexer.token = Token::Error;
		break;
	}

	return lexer.token;
}

CharType GetCharType(wchar_t ch)
{
	switch (ch)
	{
	case L'\0':
		return CharType::Final;

	case L' ':
	case L'\t':
	case L'\n':
		return CharType::Separator;

	case L'-':
		return CharType::MinusSymbol;

	case L'+':
	case L'/':
	case L'*':
	case L'~':
	case L'(':
	case L')':
	case L'<':
	case L'>':
	case L'%':
	case L'$':
	case L',':
	case L'?':
	case L':':
	case L'=':
	case L'&':
	case L'|':
	case L'^':
		return CharType::Symbol;
	}

	if (iswdigit(ch)) return CharType::Digit;

	// Make sure this is the last character test before "Unknown".
	// This will catch all characters with graphical representation and treat them as a "Letter".
	// This includes all "alpha" characters and the following characers not defined above: _\`!#@{}[]'";
	if (iswgraph(ch)) return CharType::Letter;

	return CharType::Unknown;
}

bool IsDelimiter(wchar_t ch)
{
	CharType type = GetCharType(ch);
	return type == CharType::MinusSymbol || type == CharType::Symbol || type == CharType::Separator;
}

uint8_t GetFunctionIndex(const wchar_t* str, uint8_t len)
{
	const int funcCount = sizeof(g_Functions) / sizeof(Function);
	for (int i = 0; i < funcCount; ++i)
	{
		if (g_Functions[i].length == len &&
			wcsncasecmp(str, g_Functions[i].name, len) == 0)
		{
			return i;
		}
	}

	return FUNC_INVALID;
}

Operator GetOperator(const wchar_t* str)
{
	switch (str[0])
	{
	case L'(':
		return Operator::OpeningBracket;

	case L'+':
		return Operator::Addition;

	case L'-':
		return Operator::Subtraction;

	case L'*':
		return (str[1] == L'*') ? Operator::Power : Operator::Multiplication;

	case L'/':
		return Operator::Division;

	case L'%':
		return Operator::Modulo;

	case L'$':
		return Operator::UNK;

	case L'^':
		return Operator::BitwiseXOR;

	case L'~':
		return Operator::BitwiseNOT;

	case L'&':
		return (str[1] == L'&') ? Operator::LogicalAND : Operator::BitwiseAND;

	case L'|':
		return (str[1] == L'|') ? Operator::LogicalOR : Operator::BitwiseOR;

	case L'=':
		return Operator::Equal;

	case L'>':
		return (str[1] == L'>') ? Operator::ShiftRight : (str[1] == L'=') ? Operator::GreatorOrEqual : Operator::Greater;

	case L'<':
		return (str[1] == L'>') ? Operator::NotEqual : (str[1] == L'<') ? Operator::ShiftLeft : (str[1] == L'=') ? Operator::LessOrEqual : Operator::Less;

	case L'?':
		return Operator::Conditional;

	case L':':
		return Operator::ConditionalSeparator;

	case L')':
		return Operator::ClosingBracket;

	case L',':
		return Operator::Comma;
	}

	return Operator::Invalid;
}

// -----------------------------------------------------------------------------------------------
//  Misc
// -----------------------------------------------------------------------------------------------

static double frac(double x)
{
	double y;
	return modf(x, &y);
}

static double rad(double deg)
{
	return (deg / 180.0) * kPi;
}

static double deg(double rad)
{
	return rad * (180.0 / kPi);
}

static double sgn(double x)
{
	return (x > 0.0) ? 1.0 : (x < 0.0) ? -1.0 : 0.0;
}

static double neg(double x)
{
	return -x;
}

static const wchar_t* Min(int paramcnt, double* args, double* result)
{
	if (paramcnt == 2)
	{
		const double& a = args[0];
		const double& b = args[1];

		*result = (a < b) ? a : b;
		return nullptr;
	}
	return eInvPrmCnt;
}

static const wchar_t* Max(int paramcnt, double* args, double* result)
{
	if (paramcnt == 2)
	{
		const double& a = args[0];
		const double& b = args[1];

		*result = (a > b) ? a : b;
		return nullptr;
	}
	return eInvPrmCnt;
}

static const wchar_t* Clamp(int paramcnt, double* args, double* result)
{
	if (paramcnt == 3)
	{
		const double& x = args[0];
		const double& a = args[1];
		const double& b = args[2];

		*result = (x < a) ? a : ((x > b) ? b : x);
		return nullptr;
	}
	return eInvPrmCnt;
}

// "Advanced" round function; second argument - sharpness
static const wchar_t* round(int paramcnt, double* args, double* result)
{
	int sharpness;
	if (paramcnt == 1)
	{
		sharpness = 0;
	}
	else if (paramcnt == 2)
	{
		sharpness = (int)args[1];
	}
	else
	{
		return eInvPrmCnt;
	}

	double x = args[0];
	double coef = 10.0;
	if (sharpness < 0)
	{
		coef = 0.1;
		sharpness = -sharpness;
	}

	for (int i = 0; i < sharpness; i++) x *= coef;

	x = (x + ((x >= 0.0) ? 0.5 : -0.5));
	x = (x >= 0.0) ? floor(x) : ceil(x);

	for (int i = 0; i < sharpness; i++) x /= coef;

	*result = x;
	return nullptr;
}

// wrapper for standard math lib atan2
static const wchar_t* ATan2(int paramcnt, double* args, double* result)
{
	if (paramcnt == 2)
	{
		const double& y = args[0];
		const double& x = args[1];

		*result = atan2(y, x);
		return nullptr;
	}
	return eInvPrmCnt;
}

}  // namespace MathParser
