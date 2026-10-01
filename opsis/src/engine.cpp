#include "opsis/interpreter.h"

#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

namespace Opsis {
namespace {

struct RuntimeError : std::runtime_error {
	int Code;
	bool Printed;

	RuntimeError(int InCode, const std::string &Message, bool InPrinted)
		: std::runtime_error(Message), Code(InCode), Printed(InPrinted) {}
};

class Interpreter;

enum class ValueKind {
	Null,
	Int,
	Bool,
	String,
	Native,
	Function,
	Module
};

struct Function;
struct Module;

class Value {
public:
	Value() = default;

	static Value Null() { return Value(); }
	static Value FromInt(int64_t Number);
	static Value FromBool(bool Flag);
	static Value FromString(std::string Text);
	static Value FromNative(std::string Name, int MinArgs, int MaxArgs,
		std::function<Value(Interpreter &, const std::vector<Value> &)> Fn);
	static Value FromFunction(std::shared_ptr<Function> Fn);
	static Value FromModule(std::shared_ptr<Module> Mod);

	ValueKind Kind() const { return Kind_; }
	bool IsNull() const { return Kind_ == ValueKind::Null; }
	bool IsInt() const { return Kind_ == ValueKind::Int; }
	bool IsBool() const { return Kind_ == ValueKind::Bool; }
	bool IsString() const { return Kind_ == ValueKind::String; }
	bool IsNative() const { return Kind_ == ValueKind::Native; }
	bool IsFunction() const { return Kind_ == ValueKind::Function; }
	bool IsModule() const { return Kind_ == ValueKind::Module; }

	int64_t Int() const { return Int_; }
	bool Bool() const { return Bool_; }
	const std::string &Text() const { return Text_; }
	const std::string &NativeName() const { return NativeName_; }
	int NativeMin() const { return NativeMin_; }
	int NativeMax() const { return NativeMax_; }
	const std::function<Value(Interpreter &, const std::vector<Value> &)> &NativeFn() const { return NativeFn_; }
	const std::shared_ptr<Function> &FunctionPtr() const { return Function_; }
	const std::shared_ptr<Module> &ModulePtr() const { return Module_; }

	std::string ToString() const;

private:
	ValueKind Kind_{ValueKind::Null};
	int64_t Int_{0};
	bool Bool_{false};
	std::string Text_;
	std::string NativeName_;
	int NativeMin_{0};
	int NativeMax_{0};
	std::function<Value(Interpreter &, const std::vector<Value> &)> NativeFn_;
	std::shared_ptr<Function> Function_;
	std::shared_ptr<Module> Module_;
};

struct Module {
	std::unordered_map<std::string, Value> Members;
};

struct ReturnSignal {
	Value Result;
};

class Environment {
public:
	explicit Environment(Environment *Parent) : Parent_(Parent) {}

	void Define(const std::string &Name, Value Item)
	{
		if (Values_.count(Name)) {
			throw RuntimeError(2, "name already defined: " + Name, false);
		}
		Values_[Name] = std::move(Item);
	}

	void Assign(const std::string &Name, Value Item)
	{
		if (Values_.count(Name)) {
			Values_[Name] = std::move(Item);
			return;
		}
		if (Parent_) {
			Parent_->Assign(Name, std::move(Item));
			return;
		}
		throw RuntimeError(2, "name is not defined: " + Name, false);
	}

	Value Get(const std::string &Name) const
	{
		auto Found = Values_.find(Name);
		if (Found != Values_.end()) return Found->second;
		if (Parent_) return Parent_->Get(Name);
		throw RuntimeError(2, "name is not defined: " + Name, false);
	}

private:
	Environment *Parent_;
	std::unordered_map<std::string, Value> Values_;
};

class Expr {
public:
	explicit Expr(int Line) : Line_(Line) {}
	virtual ~Expr() = default;
	virtual Value Evaluate(Interpreter &Machine) const = 0;
	int Line() const { return Line_; }

private:
	int Line_;
};

class Stmt {
public:
	explicit Stmt(int Line) : Line_(Line) {}
	virtual ~Stmt() = default;
	virtual void Execute(Interpreter &Machine) const = 0;
	int Line() const { return Line_; }

private:
	int Line_;
};

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

struct Function {
	std::string Name;
	std::string ReturnType;
	std::vector<std::pair<std::string, std::string>> Params;
	const Stmt *Body{nullptr};
};

enum class TokenKind {
	End, Ident, Int, String,
	KwTrue, KwFalse, KwVoid, KwInt, KwString, KwBool, KwVar,
	KwIf, KwElse, KwWhile, KwFor, KwReturn, KwClass, KwPublic, KwStatic,
	LParen, RParen, LBrace, RBrace, Comma, Semicolon, Dot, Assign,
	Plus, Minus, Star, Slash, Percent, Bang,
	EqualEqual, BangEqual, Less, Greater, LessEqual, GreaterEqual,
	AmpAmp, PipePipe
};

struct Token {
	TokenKind Kind{TokenKind::End};
	std::string Text;
	int64_t IntValue{0};
	int Line{1};
};

class LiteralExpr : public Expr {
public:
	LiteralExpr(int Line, Value Item) : Expr(Line), Item_(std::move(Item)) {}
	Value Evaluate(Interpreter &) const override { return Item_; }

private:
	Value Item_;
};

class VariableExpr : public Expr {
public:
	VariableExpr(int Line, std::string Name) : Expr(Line), Name_(std::move(Name)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	std::string Name_;
};

class AssignExpr : public Expr {
public:
	AssignExpr(int Line, std::string Name, ExprPtr ValueExpr)
		: Expr(Line), Name_(std::move(Name)), ValueExpr_(std::move(ValueExpr)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	std::string Name_;
	ExprPtr ValueExpr_;
};

class UnaryExpr : public Expr {
public:
	UnaryExpr(int Line, TokenKind Op, ExprPtr Right)
		: Expr(Line), Op_(Op), Right_(std::move(Right)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	TokenKind Op_;
	ExprPtr Right_;
};

class BinaryExpr : public Expr {
public:
	BinaryExpr(int Line, ExprPtr Left, TokenKind Op, ExprPtr Right)
		: Expr(Line), Left_(std::move(Left)), Op_(Op), Right_(std::move(Right)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	ExprPtr Left_;
	TokenKind Op_;
	ExprPtr Right_;
};

class CallExpr : public Expr {
public:
	CallExpr(int Line, ExprPtr Callee, std::vector<ExprPtr> Args)
		: Expr(Line), Callee_(std::move(Callee)), Args_(std::move(Args)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	ExprPtr Callee_;
	std::vector<ExprPtr> Args_;
};

class GetExpr : public Expr {
public:
	GetExpr(int Line, ExprPtr Object, std::string Name)
		: Expr(Line), Object_(std::move(Object)), Name_(std::move(Name)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	ExprPtr Object_;
	std::string Name_;
};

class ExprStmt : public Stmt {
public:
	ExprStmt(int Line, ExprPtr Item) : Stmt(Line), Item_(std::move(Item)) {}
	void Execute(Interpreter &Machine) const override;

private:
	ExprPtr Item_;
};

class VarStmt : public Stmt {
public:
	VarStmt(int Line, std::string TypeName, std::string Name, ExprPtr Init)
		: Stmt(Line), TypeName_(std::move(TypeName)), Name_(std::move(Name)), Init_(std::move(Init)) {}
	void Execute(Interpreter &Machine) const override;

private:
	std::string TypeName_;
	std::string Name_;
	ExprPtr Init_;
};

class BlockStmt : public Stmt {
public:
	BlockStmt(int Line, std::vector<StmtPtr> Items) : Stmt(Line), Items_(std::move(Items)) {}
	void Execute(Interpreter &Machine) const override;

private:
	std::vector<StmtPtr> Items_;
};

class IfStmt : public Stmt {
public:
	IfStmt(int Line, ExprPtr Cond, StmtPtr ThenArm, StmtPtr ElseArm)
		: Stmt(Line), Cond_(std::move(Cond)), ThenArm_(std::move(ThenArm)), ElseArm_(std::move(ElseArm)) {}
	void Execute(Interpreter &Machine) const override;

private:
	ExprPtr Cond_;
	StmtPtr ThenArm_;
	StmtPtr ElseArm_;
};

class WhileStmt : public Stmt {
public:
	WhileStmt(int Line, ExprPtr Cond, StmtPtr Body)
		: Stmt(Line), Cond_(std::move(Cond)), Body_(std::move(Body)) {}
	void Execute(Interpreter &Machine) const override;

private:
	ExprPtr Cond_;
	StmtPtr Body_;
};

class ForStmt : public Stmt {
public:
	ForStmt(int Line, StmtPtr Init, ExprPtr Cond, ExprPtr Step, StmtPtr Body)
		: Stmt(Line), Init_(std::move(Init)), Cond_(std::move(Cond)), Step_(std::move(Step)), Body_(std::move(Body)) {}
	void Execute(Interpreter &Machine) const override;

private:
	StmtPtr Init_;
	ExprPtr Cond_;
	ExprPtr Step_;
	StmtPtr Body_;
};

class ReturnStmt : public Stmt {
public:
	ReturnStmt(int Line, ExprPtr ValueExpr) : Stmt(Line), ValueExpr_(std::move(ValueExpr)) {}
	void Execute(Interpreter &Machine) const override;

private:
	ExprPtr ValueExpr_;
};

class FunctionStmt : public Stmt {
public:
	FunctionStmt(int Line, std::string Name, std::string ReturnType,
		std::vector<std::pair<std::string, std::string>> Params, StmtPtr Body)
		: Stmt(Line), Name_(std::move(Name)), ReturnType_(std::move(ReturnType)),
		Params_(std::move(Params)), Body_(std::move(Body)) {}
	void Execute(Interpreter &) const override {}
	const std::string &Name() const { return Name_; }
	const std::string &ReturnType() const { return ReturnType_; }
	const std::vector<std::pair<std::string, std::string>> &Params() const { return Params_; }
	const Stmt *Body() const { return Body_.get(); }

private:
	std::string Name_;
	std::string ReturnType_;
	std::vector<std::pair<std::string, std::string>> Params_;
	StmtPtr Body_;
};

class ClassStmt : public Stmt {
public:
	ClassStmt(int Line, std::string Name, std::vector<std::unique_ptr<FunctionStmt>> Methods)
		: Stmt(Line), Name_(std::move(Name)), Methods_(std::move(Methods)) {}
	void Execute(Interpreter &) const override {}
	const std::string &Name() const { return Name_; }
	const std::vector<std::unique_ptr<FunctionStmt>> &Methods() const { return Methods_; }

private:
	std::string Name_;
	std::vector<std::unique_ptr<FunctionStmt>> Methods_;
};

class EnvironmentGuard {
public:
	EnvironmentGuard(Interpreter &Machine, Environment *Next);
	~EnvironmentGuard();

private:
	Interpreter &Machine_;
	Environment *Previous_;
};

class Interpreter {
public:
	Interpreter(RuntimeConfig Config, std::string FileName);

	void Run(const std::vector<StmtPtr> &Items);
	void DefineFunction(const FunctionStmt *Node, const std::string &ClassName);
	[[noreturn]] void Fail(int Line, const std::string &Message, bool OpsisFormat);
	Value CallFunction(const Function &Fn, const std::vector<Value> &Args, int Line);
	Environment *Current() { return Current_; }
	void SetCurrent(Environment *Next) { Current_ = Next; }
	const RuntimeConfig &Config() const { return Config_; }
	const fs::path &ScriptDirectory() const { return ScriptDirectory_; }

private:
	RuntimeConfig Config_;
	std::string FileName_;
	fs::path ScriptDirectory_;
	Environment Globals_;
	Environment *Current_;
	std::shared_ptr<Function> Entry_;
	int EntryRank_{100};
	std::shared_ptr<Module> OpsisModule_;
};

Value Value::FromInt(int64_t Number)
{
	Value Item;
	Item.Kind_ = ValueKind::Int;
	Item.Int_ = Number;
	return Item;
}

Value Value::FromBool(bool Flag)
{
	Value Item;
	Item.Kind_ = ValueKind::Bool;
	Item.Bool_ = Flag;
	return Item;
}

Value Value::FromString(std::string Text)
{
	Value Item;
	Item.Kind_ = ValueKind::String;
	Item.Text_ = std::move(Text);
	return Item;
}

Value Value::FromNative(std::string Name, int MinArgs, int MaxArgs,
	std::function<Value(Interpreter &, const std::vector<Value> &)> Fn)
{
	Value Item;
	Item.Kind_ = ValueKind::Native;
	Item.NativeName_ = std::move(Name);
	Item.NativeMin_ = MinArgs;
	Item.NativeMax_ = MaxArgs;
	Item.NativeFn_ = std::move(Fn);
	return Item;
}

Value Value::FromFunction(std::shared_ptr<Function> Fn)
{
	Value Item;
	Item.Kind_ = ValueKind::Function;
	Item.Function_ = std::move(Fn);
	return Item;
}

Value Value::FromModule(std::shared_ptr<Module> Mod)
{
	Value Item;
	Item.Kind_ = ValueKind::Module;
	Item.Module_ = std::move(Mod);
	return Item;
}

std::string Value::ToString() const
{
	switch (Kind_) {
	case ValueKind::Null: return "null";
	case ValueKind::Int: return std::to_string(Int_);
	case ValueKind::Bool: return Bool_ ? "true" : "false";
	case ValueKind::String: return Text_;
	case ValueKind::Native: return NativeName_;
	case ValueKind::Function: return Function_ ? Function_->Name : "function";
	case ValueKind::Module: return "module";
	}
	return "";
}

EnvironmentGuard::EnvironmentGuard(Interpreter &Machine, Environment *Next)
	: Machine_(Machine), Previous_(Machine.Current())
{
	Machine_.SetCurrent(Next);
}

EnvironmentGuard::~EnvironmentGuard()
{
	Machine_.SetCurrent(Previous_);
}

class Lexer {
public:
	Lexer(std::string Source, std::string FileName)
		: Source_(std::move(Source)), FileName_(std::move(FileName)) {}

	Token Next()
	{
		SkipSpace();
		if (At_ >= Source_.size()) return Make(TokenKind::End, "");
		char Ch = Source_[At_];
		if (std::isalpha(static_cast<unsigned char>(Ch)) || Ch == '_') return Ident();
		if (std::isdigit(static_cast<unsigned char>(Ch))) return Number();
		if (Ch == '"') return String();
		if (Ch == '&' && Peek() == '&') { At_ += 2; return Make(TokenKind::AmpAmp, "&&"); }
		if (Ch == '|' && Peek() == '|') { At_ += 2; return Make(TokenKind::PipePipe, "||"); }
		if (Ch == '=' && Peek() == '=') { At_ += 2; return Make(TokenKind::EqualEqual, "=="); }
		if (Ch == '!' && Peek() == '=') { At_ += 2; return Make(TokenKind::BangEqual, "!="); }
		if (Ch == '<' && Peek() == '=') { At_ += 2; return Make(TokenKind::LessEqual, "<="); }
		if (Ch == '>' && Peek() == '=') { At_ += 2; return Make(TokenKind::GreaterEqual, ">="); }
		At_++;
		switch (Ch) {
		case '(': return Make(TokenKind::LParen, "(");
		case ')': return Make(TokenKind::RParen, ")");
		case '{': return Make(TokenKind::LBrace, "{");
		case '}': return Make(TokenKind::RBrace, "}");
		case ',': return Make(TokenKind::Comma, ",");
		case ';': return Make(TokenKind::Semicolon, ";");
		case '.': return Make(TokenKind::Dot, ".");
		case '=': return Make(TokenKind::Assign, "=");
		case '+': return Make(TokenKind::Plus, "+");
		case '-': return Make(TokenKind::Minus, "-");
		case '*': return Make(TokenKind::Star, "*");
		case '/': return Make(TokenKind::Slash, "/");
		case '%': return Make(TokenKind::Percent, "%");
		case '!': return Make(TokenKind::Bang, "!");
		case '<': return Make(TokenKind::Less, "<");
		case '>': return Make(TokenKind::Greater, ">");
		default:
			throw RuntimeError(2, Where() + "unexpected character", false);
		}
	}

private:
	Token Make(TokenKind Kind, std::string Text) const
	{
		Token Item;
		Item.Kind = Kind;
		Item.Text = std::move(Text);
		Item.Line = Line_;
		return Item;
	}

	std::string Where() const
	{
		return FileName_ + ":" + std::to_string(Line_) + ": ";
	}

	char Peek() const
	{
		if (At_ + 1 >= Source_.size()) return '\0';
		return Source_[At_ + 1];
	}

	void SkipSpace()
	{
		while (At_ < Source_.size()) {
			char Ch = Source_[At_];
			if (Ch == ' ' || Ch == '\t' || Ch == '\r') {
				At_++;
				continue;
			}
			if (Ch == '\n') {
				At_++;
				Line_++;
				continue;
			}
			if (Ch == '/' && Peek() == '/') {
				while (At_ < Source_.size() && Source_[At_] != '\n') At_++;
				continue;
			}
			if (Ch == '/' && Peek() == '*') {
				At_ += 2;
				while (At_ + 1 < Source_.size() && !(Source_[At_] == '*' && Source_[At_ + 1] == '/')) {
					if (Source_[At_] == '\n') Line_++;
					At_++;
				}
				if (At_ + 1 >= Source_.size()) {
					throw RuntimeError(2, Where() + "unclosed comment", false);
				}
				At_ += 2;
				continue;
			}
			break;
		}
	}

	Token Ident()
	{
		size_t Start = At_;
		while (At_ < Source_.size()) {
			unsigned char Ch = static_cast<unsigned char>(Source_[At_]);
			if (!std::isalnum(Ch) && Source_[At_] != '_') break;
			At_++;
		}
		std::string Text = Source_.substr(Start, At_ - Start);
		static const std::unordered_map<std::string, TokenKind> Keywords = {
			{"true", TokenKind::KwTrue}, {"false", TokenKind::KwFalse},
			{"void", TokenKind::KwVoid}, {"int", TokenKind::KwInt},
			{"string", TokenKind::KwString}, {"bool", TokenKind::KwBool},
			{"var", TokenKind::KwVar}, {"if", TokenKind::KwIf},
			{"else", TokenKind::KwElse}, {"while", TokenKind::KwWhile},
			{"for", TokenKind::KwFor}, {"return", TokenKind::KwReturn},
			{"class", TokenKind::KwClass}, {"public", TokenKind::KwPublic},
			{"static", TokenKind::KwStatic}
		};
		auto Found = Keywords.find(Text);
		if (Found != Keywords.end()) return Make(Found->second, Text);
		return Make(TokenKind::Ident, Text);
	}

	Token Number()
	{
		size_t Start = At_;
		int Base = 10;
		if (Source_[At_] == '0' && At_ + 1 < Source_.size() && (Source_[At_ + 1] == 'x' || Source_[At_ + 1] == 'X')) {
			Base = 16;
			At_ += 2;
			Start = At_;
			while (At_ < Source_.size() && std::isxdigit(static_cast<unsigned char>(Source_[At_]))) At_++;
		} else {
			while (At_ < Source_.size() && std::isdigit(static_cast<unsigned char>(Source_[At_]))) At_++;
		}
		std::string Text = Source_.substr(Start, At_ - Start);
		if (Text.empty()) throw RuntimeError(2, Where() + "bad number", false);
		try {
			Token Item = Make(TokenKind::Int, Text);
			Item.IntValue = std::stoll(Text, nullptr, Base);
			return Item;
		} catch (...) {
			throw RuntimeError(2, Where() + "integer out of range", false);
		}
	}

	Token String()
	{
		At_++;
		std::string Text;
		while (At_ < Source_.size() && Source_[At_] != '"') {
			if (Source_[At_] == '\n') throw RuntimeError(2, Where() + "unterminated string", false);
			if (Source_[At_] == '\\') {
				At_++;
				if (At_ >= Source_.size()) throw RuntimeError(2, Where() + "unterminated string", false);
				char Esc = Source_[At_++];
				if (Esc == 'n') Text.push_back('\n');
				else if (Esc == 't') Text.push_back('\t');
				else if (Esc == 'r') Text.push_back('\r');
				else if (Esc == '\\' || Esc == '"') Text.push_back(Esc);
				else throw RuntimeError(2, Where() + "unknown string escape", false);
				continue;
			}
			Text.push_back(Source_[At_++]);
		}
		if (At_ >= Source_.size() || Source_[At_] != '"') {
			throw RuntimeError(2, Where() + "unterminated string", false);
		}
		At_++;
		return Make(TokenKind::String, Text);
	}

	std::string Source_;
	std::string FileName_;
	size_t At_{0};
	int Line_{1};
};

class Parser {
public:
	Parser(std::string Source, std::string FileName)
		: Lexer_(std::move(Source), FileName), FileName_(std::move(FileName))
	{
		Advance();
		Advance();
		Advance();
	}

	std::vector<StmtPtr> Parse()
	{
		std::vector<StmtPtr> Items;
		while (!Check(TokenKind::End)) {
			SkipModifiers();
			if (Check(TokenKind::KwClass)) Items.push_back(ParseClass());
			else if (IsType(Current_.Kind) && Upcoming_.Kind == TokenKind::Ident && Third_.Kind == TokenKind::LParen) {
				Items.push_back(ParseFunction());
			} else {
				Items.push_back(ParseStatement());
			}
		}
		return Items;
	}

private:
	void Advance()
	{
		Current_ = Upcoming_;
		Upcoming_ = Third_;
		Third_ = Lexer_.Next();
	}

	bool Check(TokenKind Kind) const { return Current_.Kind == Kind; }

	bool Match(TokenKind Kind)
	{
		if (!Check(Kind)) return false;
		Advance();
		return true;
	}

	void Expect(TokenKind Kind, const std::string &What)
	{
		if (!Match(Kind)) throw RuntimeError(2, Where() + "expected " + What, false);
	}

	std::string Where() const
	{
		return FileName_ + ":" + std::to_string(Current_.Line) + ": ";
	}

	void SkipModifiers()
	{
		while (Check(TokenKind::KwPublic) || Check(TokenKind::KwStatic)) Advance();
	}

	bool IsType(TokenKind Kind) const
	{
		return Kind == TokenKind::KwVoid || Kind == TokenKind::KwInt || Kind == TokenKind::KwString
			|| Kind == TokenKind::KwBool || Kind == TokenKind::KwVar;
	}

	std::string TypeText(TokenKind Kind) const
	{
		if (Kind == TokenKind::KwVoid) return "void";
		if (Kind == TokenKind::KwInt) return "int";
		if (Kind == TokenKind::KwString) return "string";
		if (Kind == TokenKind::KwBool) return "bool";
		if (Kind == TokenKind::KwVar) return "var";
		return "";
	}

	StmtPtr ParseClass()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwClass, "class");
		if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected class name", false);
		std::string Name = Current_.Text;
		Advance();
		Expect(TokenKind::LBrace, "'{'");
		std::vector<std::unique_ptr<FunctionStmt>> Methods;
		while (!Check(TokenKind::RBrace) && !Check(TokenKind::End)) {
			SkipModifiers();
			if (!IsType(Current_.Kind)) throw RuntimeError(2, Where() + "expected a method", false);
			auto Node = ParseFunction();
			auto *Fn = dynamic_cast<FunctionStmt *>(Node.get());
			if (!Fn) throw RuntimeError(2, Where() + "expected a method", false);
			Node.release();
			Methods.emplace_back(Fn);
		}
		Expect(TokenKind::RBrace, "'}'");
		return std::make_unique<ClassStmt>(Line, Name, std::move(Methods));
	}

	StmtPtr ParseFunction()
	{
		int Line = Current_.Line;
		if (!IsType(Current_.Kind)) throw RuntimeError(2, Where() + "expected a return type", false);
		std::string ReturnType = TypeText(Current_.Kind);
		Advance();
		if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected a method name", false);
		std::string Name = Current_.Text;
		Advance();
		Expect(TokenKind::LParen, "'('");
		std::vector<std::pair<std::string, std::string>> Params;
		if (!Check(TokenKind::RParen)) {
			do {
				if (!IsType(Current_.Kind) || Current_.Kind == TokenKind::KwVoid || Current_.Kind == TokenKind::KwVar) {
					throw RuntimeError(2, Where() + "expected a parameter type", false);
				}
				std::string ParamType = TypeText(Current_.Kind);
				Advance();
				if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected a parameter name", false);
				std::string ParamName = Current_.Text;
				Advance();
				Params.emplace_back(ParamType, ParamName);
			} while (Match(TokenKind::Comma));
		}
		Expect(TokenKind::RParen, "')'");
		StmtPtr Body = ParseBlock();
		return std::make_unique<FunctionStmt>(Line, Name, ReturnType, std::move(Params), std::move(Body));
	}

	StmtPtr ParseStatement()
	{
		SkipModifiers();
		if (Check(TokenKind::LBrace)) return ParseBlock();
		if (Check(TokenKind::KwIf)) return ParseIf();
		if (Check(TokenKind::KwWhile)) return ParseWhile();
		if (Check(TokenKind::KwFor)) return ParseFor();
		if (Check(TokenKind::KwReturn)) return ParseReturn();
		if (IsVarType(Current_.Kind)) return ParseVar();
		int Line = Current_.Line;
		ExprPtr Item = ParseExpression();
		Expect(TokenKind::Semicolon, "';'");
		return std::make_unique<ExprStmt>(Line, std::move(Item));
	}

	bool IsVarType(TokenKind Kind) const
	{
		return Kind == TokenKind::KwInt || Kind == TokenKind::KwString || Kind == TokenKind::KwBool
			|| Kind == TokenKind::KwVar;
	}

	StmtPtr ParseBlock()
	{
		int Line = Current_.Line;
		Expect(TokenKind::LBrace, "'{'");
		std::vector<StmtPtr> Items;
		while (!Check(TokenKind::RBrace) && !Check(TokenKind::End)) {
			Items.push_back(ParseStatement());
		}
		Expect(TokenKind::RBrace, "'}'");
		return std::make_unique<BlockStmt>(Line, std::move(Items));
	}

	StmtPtr ParseIf()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwIf, "if");
		Expect(TokenKind::LParen, "'('");
		ExprPtr Cond = ParseExpression();
		Expect(TokenKind::RParen, "')'");
		StmtPtr ThenArm = ParseStatement();
		StmtPtr ElseArm;
		if (Match(TokenKind::KwElse)) ElseArm = ParseStatement();
		return std::make_unique<IfStmt>(Line, std::move(Cond), std::move(ThenArm), std::move(ElseArm));
	}

	StmtPtr ParseWhile()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwWhile, "while");
		Expect(TokenKind::LParen, "'('");
		ExprPtr Cond = ParseExpression();
		Expect(TokenKind::RParen, "')'");
		return std::make_unique<WhileStmt>(Line, std::move(Cond), ParseStatement());
	}

	StmtPtr ParseFor()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwFor, "for");
		Expect(TokenKind::LParen, "'('");
		StmtPtr Init = ParseVar();
		ExprPtr Cond = ParseExpression();
		Expect(TokenKind::Semicolon, "';'");
		ExprPtr Step = ParseExpression();
		Expect(TokenKind::RParen, "')'");
		return std::make_unique<ForStmt>(Line, std::move(Init), std::move(Cond), std::move(Step), ParseStatement());
	}

	StmtPtr ParseReturn()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwReturn, "return");
		ExprPtr ValueExpr;
		if (!Check(TokenKind::Semicolon)) ValueExpr = ParseExpression();
		Expect(TokenKind::Semicolon, "';'");
		return std::make_unique<ReturnStmt>(Line, std::move(ValueExpr));
	}

	StmtPtr ParseVar()
	{
		int Line = Current_.Line;
		if (!IsVarType(Current_.Kind)) throw RuntimeError(2, Where() + "expected a variable type", false);
		std::string TypeName = TypeText(Current_.Kind);
		Advance();
		if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected a variable name", false);
		std::string Name = Current_.Text;
		Advance();
		ExprPtr Init;
		if (Match(TokenKind::Assign)) Init = ParseExpression();
		else if (TypeName == "var") throw RuntimeError(2, Where() + "var requires an initializer", false);
		Expect(TokenKind::Semicolon, "';'");
		return std::make_unique<VarStmt>(Line, TypeName, Name, std::move(Init));
	}

	ExprPtr ParseExpression() { return ParseAssignment(); }

	ExprPtr ParseAssignment()
	{
		if (Current_.Kind == TokenKind::Ident && Upcoming_.Kind == TokenKind::Assign) {
			int Line = Current_.Line;
			std::string Name = Current_.Text;
			Advance();
			Advance();
			return std::make_unique<AssignExpr>(Line, Name, ParseAssignment());
		}
		return ParseOr();
	}

	ExprPtr ParseOr()
	{
		ExprPtr Left = ParseAnd();
		while (Current_.Kind == TokenKind::PipePipe) {
			int Line = Current_.Line;
			Advance();
			Left = std::make_unique<BinaryExpr>(Line, std::move(Left), TokenKind::PipePipe, ParseAnd());
		}
		return Left;
	}

	ExprPtr ParseAnd()
	{
		ExprPtr Left = ParseEquality();
		while (Current_.Kind == TokenKind::AmpAmp) {
			int Line = Current_.Line;
			Advance();
			Left = std::make_unique<BinaryExpr>(Line, std::move(Left), TokenKind::AmpAmp, ParseEquality());
		}
		return Left;
	}

	ExprPtr ParseEquality()
	{
		ExprPtr Left = ParseComparison();
		while (Current_.Kind == TokenKind::EqualEqual || Current_.Kind == TokenKind::BangEqual) {
			TokenKind Op = Current_.Kind;
			int Line = Current_.Line;
			Advance();
			Left = std::make_unique<BinaryExpr>(Line, std::move(Left), Op, ParseComparison());
		}
		return Left;
	}

	ExprPtr ParseComparison()
	{
		ExprPtr Left = ParseTerm();
		while (Current_.Kind == TokenKind::Less || Current_.Kind == TokenKind::Greater
			|| Current_.Kind == TokenKind::LessEqual || Current_.Kind == TokenKind::GreaterEqual) {
			TokenKind Op = Current_.Kind;
			int Line = Current_.Line;
			Advance();
			Left = std::make_unique<BinaryExpr>(Line, std::move(Left), Op, ParseTerm());
		}
		return Left;
	}

	ExprPtr ParseTerm()
	{
		ExprPtr Left = ParseFactor();
		while (Current_.Kind == TokenKind::Plus || Current_.Kind == TokenKind::Minus) {
			TokenKind Op = Current_.Kind;
			int Line = Current_.Line;
			Advance();
			Left = std::make_unique<BinaryExpr>(Line, std::move(Left), Op, ParseFactor());
		}
		return Left;
	}

	ExprPtr ParseFactor()
	{
		ExprPtr Left = ParseUnary();
		while (Current_.Kind == TokenKind::Star || Current_.Kind == TokenKind::Slash
			|| Current_.Kind == TokenKind::Percent) {
			TokenKind Op = Current_.Kind;
			int Line = Current_.Line;
			Advance();
			Left = std::make_unique<BinaryExpr>(Line, std::move(Left), Op, ParseUnary());
		}
		return Left;
	}

	ExprPtr ParseUnary()
	{
		if (Current_.Kind == TokenKind::Bang || Current_.Kind == TokenKind::Minus) {
			TokenKind Op = Current_.Kind;
			int Line = Current_.Line;
			Advance();
			return std::make_unique<UnaryExpr>(Line, Op, ParseUnary());
		}
		return ParseCall();
	}

	ExprPtr ParseCall()
	{
		ExprPtr Item = ParsePrimary();
		while (true) {
			if (Match(TokenKind::LParen)) {
				int Line = Item->Line();
				std::vector<ExprPtr> Args;
				if (!Check(TokenKind::RParen)) {
					do {
						Args.push_back(ParseExpression());
					} while (Match(TokenKind::Comma));
				}
				Expect(TokenKind::RParen, "')'");
				Item = std::make_unique<CallExpr>(Line, std::move(Item), std::move(Args));
			} else if (Match(TokenKind::Dot)) {
				int Line = Item->Line();
				if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected a member name", false);
				std::string Name = Current_.Text;
				Advance();
				Item = std::make_unique<GetExpr>(Line, std::move(Item), Name);
			} else {
				break;
			}
		}
		return Item;
	}

	ExprPtr ParsePrimary()
	{
		int Line = Current_.Line;
		if (Match(TokenKind::KwTrue)) return std::make_unique<LiteralExpr>(Line, Value::FromBool(true));
		if (Match(TokenKind::KwFalse)) return std::make_unique<LiteralExpr>(Line, Value::FromBool(false));
		if (Check(TokenKind::Int)) {
			int64_t Number = Current_.IntValue;
			Advance();
			return std::make_unique<LiteralExpr>(Line, Value::FromInt(Number));
		}
		if (Check(TokenKind::String)) {
			std::string Text = Current_.Text;
			Advance();
			return std::make_unique<LiteralExpr>(Line, Value::FromString(Text));
		}
		if (Check(TokenKind::Ident)) {
			std::string Name = Current_.Text;
			Advance();
			return std::make_unique<VariableExpr>(Line, Name);
		}
		if (Match(TokenKind::LParen)) {
			ExprPtr Inner = ParseExpression();
			Expect(TokenKind::RParen, "')'");
			return Inner;
		}
		throw RuntimeError(2, Where() + "expected an expression", false);
	}

	Lexer Lexer_;
	std::string FileName_;
	Token Current_;
	Token Upcoming_;
	Token Third_;
};

int64_t NeedInt(Interpreter &Machine, const Value &Item, int Line)
{
	if (!Item.IsInt()) Machine.Fail(Line, "expected int, got " + Item.ToString(), false);
	return Item.Int();
}

const std::string &NeedString(Interpreter &Machine, const Value &Item, int Line)
{
	if (!Item.IsString()) Machine.Fail(Line, "expected string", false);
	return Item.Text();
}

bool NeedBool(Interpreter &Machine, const Value &Item, int Line)
{
	if (!Item.IsBool()) Machine.Fail(Line, "expected bool", false);
	return Item.Bool();
}

bool TypeOk(const std::string &TypeName, const Value &Item)
{
	if (TypeName == "var") return true;
	if (TypeName == "int") return Item.IsInt();
	if (TypeName == "string") return Item.IsString();
	if (TypeName == "bool") return Item.IsBool();
	if (TypeName == "void") return Item.IsNull();
	return false;
}

Value VariableExpr::Evaluate(Interpreter &Machine) const
{
	try {
		return Machine.Current()->Get(Name_);
	} catch (const RuntimeError &Error) {
		Machine.Fail(Line(), Error.what(), false);
	}
}

Value AssignExpr::Evaluate(Interpreter &Machine) const
{
	Value Item = ValueExpr_->Evaluate(Machine);
	try {
		Machine.Current()->Assign(Name_, Item);
	} catch (const RuntimeError &Error) {
		Machine.Fail(Line(), Error.what(), false);
	}
	return Item;
}

Value UnaryExpr::Evaluate(Interpreter &Machine) const
{
	Value Item = Right_->Evaluate(Machine);
	if (Op_ == TokenKind::Bang) return Value::FromBool(!NeedBool(Machine, Item, Line()));
	if (Op_ == TokenKind::Minus) return Value::FromInt(-NeedInt(Machine, Item, Line()));
	Machine.Fail(Line(), "unknown unary operator", false);
}

Value BinaryExpr::Evaluate(Interpreter &Machine) const
{
	if (Op_ == TokenKind::PipePipe) {
		Value Left = Left_->Evaluate(Machine);
		if (NeedBool(Machine, Left, Line())) return Left;
		return Value::FromBool(NeedBool(Machine, Right_->Evaluate(Machine), Line()));
	}
	if (Op_ == TokenKind::AmpAmp) {
		Value Left = Left_->Evaluate(Machine);
		if (!NeedBool(Machine, Left, Line())) return Left;
		return Value::FromBool(NeedBool(Machine, Right_->Evaluate(Machine), Line()));
	}
	Value Left = Left_->Evaluate(Machine);
	Value Right = Right_->Evaluate(Machine);
	if (Op_ == TokenKind::Plus && (Left.IsString() || Right.IsString())) {
		return Value::FromString(Left.ToString() + Right.ToString());
	}
	if (Op_ == TokenKind::Plus || Op_ == TokenKind::Minus || Op_ == TokenKind::Star
		|| Op_ == TokenKind::Slash || Op_ == TokenKind::Percent) {
		int64_t A = NeedInt(Machine, Left, Line());
		int64_t B = NeedInt(Machine, Right, Line());
		if ((Op_ == TokenKind::Slash || Op_ == TokenKind::Percent) && B == 0) {
			Machine.Fail(Line(), "division by zero", false);
		}
		if (Op_ == TokenKind::Plus) return Value::FromInt(A + B);
		if (Op_ == TokenKind::Minus) return Value::FromInt(A - B);
		if (Op_ == TokenKind::Star) return Value::FromInt(A * B);
		if (Op_ == TokenKind::Slash) return Value::FromInt(A / B);
		return Value::FromInt(A % B);
	}
	if (Left.Kind() != Right.Kind()) Machine.Fail(Line(), "cannot compare different types", false);
	if (Left.IsInt()) {
		bool Flag = false;
		if (Op_ == TokenKind::EqualEqual) Flag = Left.Int() == Right.Int();
		else if (Op_ == TokenKind::BangEqual) Flag = Left.Int() != Right.Int();
		else if (Op_ == TokenKind::Less) Flag = Left.Int() < Right.Int();
		else if (Op_ == TokenKind::Greater) Flag = Left.Int() > Right.Int();
		else if (Op_ == TokenKind::LessEqual) Flag = Left.Int() <= Right.Int();
		else if (Op_ == TokenKind::GreaterEqual) Flag = Left.Int() >= Right.Int();
		else Machine.Fail(Line(), "unknown operator", false);
		return Value::FromBool(Flag);
	}
	if (Left.IsString()) {
		int Cmp = Left.Text().compare(Right.Text());
		bool Flag = false;
		if (Op_ == TokenKind::EqualEqual) Flag = Cmp == 0;
		else if (Op_ == TokenKind::BangEqual) Flag = Cmp != 0;
		else if (Op_ == TokenKind::Less) Flag = Cmp < 0;
		else if (Op_ == TokenKind::Greater) Flag = Cmp > 0;
		else if (Op_ == TokenKind::LessEqual) Flag = Cmp <= 0;
		else if (Op_ == TokenKind::GreaterEqual) Flag = Cmp >= 0;
		else Machine.Fail(Line(), "unknown operator", false);
		return Value::FromBool(Flag);
	}
	if (Left.IsBool() && (Op_ == TokenKind::EqualEqual || Op_ == TokenKind::BangEqual)) {
		bool Flag = Left.Bool() == Right.Bool();
		if (Op_ == TokenKind::BangEqual) Flag = !Flag;
		return Value::FromBool(Flag);
	}
	Machine.Fail(Line(), "unsupported operands", false);
}

Value CallExpr::Evaluate(Interpreter &Machine) const
{
	Value Callee = Callee_->Evaluate(Machine);
	std::vector<Value> Args;
	for (const auto &Arg : Args_) Args.push_back(Arg->Evaluate(Machine));
	if (Callee.IsNative()) {
		if (static_cast<int>(Args.size()) < Callee.NativeMin() || static_cast<int>(Args.size()) > Callee.NativeMax()) {
			Machine.Fail(Line(), Callee.NativeName() + " argument count mismatch", false);
		}
		return Callee.NativeFn()(Machine, Args);
	}
	if (Callee.IsFunction()) return Machine.CallFunction(*Callee.FunctionPtr(), Args, Line());
	Machine.Fail(Line(), "value is not callable", false);
}

Value GetExpr::Evaluate(Interpreter &Machine) const
{
	Value Object = Object_->Evaluate(Machine);
	if (!Object.IsModule()) Machine.Fail(Line(), "member access needs a class", false);
	auto Found = Object.ModulePtr()->Members.find(Name_);
	if (Found == Object.ModulePtr()->Members.end()) {
		Machine.Fail(Line(), "no member " + Name_, false);
	}
	return Found->second;
}

void ExprStmt::Execute(Interpreter &Machine) const
{
	Item_->Evaluate(Machine);
}

void VarStmt::Execute(Interpreter &Machine) const
{
	Value Item = Value::Null();
	if (Init_) Item = Init_->Evaluate(Machine);
	else if (TypeName_ == "int") Item = Value::FromInt(0);
	else if (TypeName_ == "bool") Item = Value::FromBool(false);
	else if (TypeName_ == "string") Item = Value::FromString("");
	if (!TypeOk(TypeName_, Item)) Machine.Fail(Line(), "initializer type does not match " + TypeName_, false);
	try {
		Machine.Current()->Define(Name_, Item);
	} catch (const RuntimeError &Error) {
		Machine.Fail(Line(), Error.what(), false);
	}
}

void BlockStmt::Execute(Interpreter &Machine) const
{
	Environment Local(Machine.Current());
	EnvironmentGuard Guard(Machine, &Local);
	for (const auto &Item : Items_) Item->Execute(Machine);
}

void IfStmt::Execute(Interpreter &Machine) const
{
	if (NeedBool(Machine, Cond_->Evaluate(Machine), Line())) ThenArm_->Execute(Machine);
	else if (ElseArm_) ElseArm_->Execute(Machine);
}

void WhileStmt::Execute(Interpreter &Machine) const
{
	while (NeedBool(Machine, Cond_->Evaluate(Machine), Line())) Body_->Execute(Machine);
}

void ForStmt::Execute(Interpreter &Machine) const
{
	Environment Local(Machine.Current());
	EnvironmentGuard Guard(Machine, &Local);
	Init_->Execute(Machine);
	while (NeedBool(Machine, Cond_->Evaluate(Machine), Line())) {
		Body_->Execute(Machine);
		if (Step_) Step_->Evaluate(Machine);
	}
}

void ReturnStmt::Execute(Interpreter &Machine) const
{
	Value Item = Value::Null();
	if (ValueExpr_) Item = ValueExpr_->Evaluate(Machine);
	throw ReturnSignal{Item};
}

void Interpreter::Fail(int Line, const std::string &Message, bool OpsisFormat)
{
	if (OpsisFormat) {
		std::cerr << "\033[1;31m[OPSIS:FAIL]\033[0m " << Message << "\n";
		throw RuntimeError(1, Message, true);
	}
	throw RuntimeError(2, FileName_ + ":" + std::to_string(Line) + ": " + Message, false);
}

Value Interpreter::CallFunction(const Function &Fn, const std::vector<Value> &Args, int Line)
{
	if (Args.size() != Fn.Params.size()) Fail(Line, Fn.Name + " argument count mismatch", false);
	Environment Local(&Globals_);
	EnvironmentGuard Guard(*this, &Local);
	for (size_t Index = 0; Index < Fn.Params.size(); ++Index) {
		if (!TypeOk(Fn.Params[Index].first, Args[Index])) {
			Fail(Line, "parameter type mismatch for " + Fn.Params[Index].second, false);
		}
		Local.Define(Fn.Params[Index].second, Args[Index]);
	}
	try {
		Fn.Body->Execute(*this);
	} catch (const ReturnSignal &Signal) {
		if (Fn.ReturnType == "void") {
			if (!Signal.Result.IsNull()) Fail(Line, "void method returned a value", false);
			return Value::Null();
		}
		if (!TypeOk(Fn.ReturnType, Signal.Result)) Fail(Line, "return type mismatch", false);
		return Signal.Result;
	}
	if (Fn.ReturnType != "void") Fail(Line, "missing return", false);
	return Value::Null();
}

namespace {

void LogLine(const char *Color, const char *Tag, const std::string &Message, std::ostream &Out)
{
	Out << Color << Tag << "\033[0m " << Message << "\n";
}

fs::path JoinInside(Interpreter &Machine, const std::string &Destination, int Line)
{
	fs::path Root = fs::path(Machine.Config().Sysroot).lexically_normal();
	fs::path Relative = fs::path(Destination).relative_path();
	if (Relative.empty()) Machine.Fail(Line, "empty destination", true);
	fs::path Full = (Root / Relative).lexically_normal();
	fs::path Back = Full.lexically_relative(Root);
	if (Back.empty() || *Back.begin() == "..") {
		Machine.Fail(Line, "destination escapes the sysroot: " + Destination, true);
	}
	return Full;
}

fs::path ResolveSource(const Interpreter &Machine, const std::string &Path)
{
	fs::path Item(Path);
	if (Item.is_absolute()) return Item;
	return Machine.ScriptDirectory() / Item;
}

int RunProcess(const std::vector<std::string> &Args, bool Quiet)
{
	pid_t Pid = fork();
	if (Pid < 0) return -1;
	if (Pid == 0) {
		if (Quiet) {
			int DevNull = open("/dev/null", O_WRONLY);
			if (DevNull >= 0) {
				dup2(DevNull, STDOUT_FILENO);
				dup2(DevNull, STDERR_FILENO);
				if (DevNull > 2) close(DevNull);
			}
		}
		std::vector<char *> Argv;
		Argv.reserve(Args.size() + 1);
		for (const auto &Arg : Args) Argv.push_back(const_cast<char *>(Arg.c_str()));
		Argv.push_back(nullptr);
		execvp(Argv[0], Argv.data());
		_exit(127);
	}
	int Status = 0;
	if (waitpid(Pid, &Status, 0) < 0) return -1;
	if (WIFEXITED(Status)) return WEXITSTATUS(Status);
	return -1;
}

void NativeCheckUser(Interpreter &Machine, int Line)
{
	if (geteuid() != 0 && !Machine.Config().AllowNonRoot) {
		Machine.Fail(Line, "OPSIS installation requires root privileges.", true);
	}
}

void NativeInstallFile(Interpreter &Machine, const std::vector<Value> &Args, int Line)
{
	std::string Source = NeedString(Machine, Args[0], Line);
	std::string Destination = NeedString(Machine, Args[1], Line);
	std::string Mode = Args.size() >= 3 ? NeedString(Machine, Args[2], Line) : "0755";
	std::string Owner = Args.size() >= 4 ? NeedString(Machine, Args[3], Line) : "0:0";
	fs::path From = ResolveSource(Machine, Source);
	fs::path To = JoinInside(Machine, Destination, Line);
	if (!fs::is_regular_file(From)) Machine.Fail(Line, "source file does not exist: " + From.string(), true);
	std::error_code Error;
	fs::create_directories(To.parent_path(), Error);
	if (Error) Machine.Fail(Line, "cannot create directory " + To.parent_path().string(), true);
	fs::copy_file(From, To, fs::copy_options::overwrite_existing, Error);
	if (Error) Machine.Fail(Line, "failed to install " + From.string(), true);
	char *End = nullptr;
	unsigned long Bits = std::strtoul(Mode.c_str(), &End, 8);
	if (End && *End == '\0' && Bits <= 07777) {
		chmod(To.c_str(), static_cast<mode_t>(Bits));
	}
	auto Colon = Owner.find(':');
	if (Colon != std::string::npos) {
		std::string User = Owner.substr(0, Colon);
		std::string Group = Owner.substr(Colon + 1);
		char *UserEnd = nullptr;
		char *GroupEnd = nullptr;
		long Uid = std::strtol(User.c_str(), &UserEnd, 10);
		long Gid = std::strtol(Group.c_str(), &GroupEnd, 10);
		if (UserEnd && *UserEnd == '\0' && GroupEnd && *GroupEnd == '\0') {
			chown(To.c_str(), static_cast<uid_t>(Uid), static_cast<gid_t>(Gid));
		}
	}
	LogLine("\033[1;32m", "[OPSIS:INFO]", "  -> installed " + Destination, std::cout);
}

void NativeInstallDirectory(Interpreter &Machine, const std::vector<Value> &Args, int Line)
{
	std::string Source = NeedString(Machine, Args[0], Line);
	std::string Prefix = Args.size() >= 2 ? NeedString(Machine, Args[1], Line) : "/";
	fs::path From = ResolveSource(Machine, Source);
	if (!fs::is_directory(From)) return;
	std::error_code Error;
	for (fs::recursive_directory_iterator It(From, fs::directory_options::skip_permission_denied, Error), End;
		It != End; It.increment(Error)) {
		if (Error) {
			Error.clear();
			continue;
		}
		fs::path Rel = fs::relative(It->path(), From, Error);
		if (Error || Rel.empty() || Rel.native().find("..") != std::string::npos) {
			Machine.Fail(Line, "payload path escapes the source directory", true);
		}
		fs::path To = JoinInside(Machine, (fs::path(Prefix) / Rel).generic_string(), Line);
		if (It->is_directory()) {
			fs::create_directories(To, Error);
			if (Error) Machine.Fail(Line, "cannot create directory " + To.string(), true);
			continue;
		}
		if (It->is_symlink()) {
			fs::create_directories(To.parent_path(), Error);
			fs::copy_symlink(It->path(), To, Error);
			if (Error) Machine.Fail(Line, "failed to copy symlink " + Rel.string(), true);
			continue;
		}
		if (!It->is_regular_file()) continue;
		fs::create_directories(To.parent_path(), Error);
		fs::copy_file(It->path(), To, fs::copy_options::overwrite_existing, Error);
		if (Error) Machine.Fail(Line, "failed to copy " + Rel.string(), true);
	}
}

void NativeRecord(Interpreter &Machine, const std::vector<Value> &Args, int Line)
{
	std::string Name = NeedString(Machine, Args[0], Line);
	std::string Version = NeedString(Machine, Args[1], Line);
	std::string Manifest = NeedString(Machine, Args[2], Line);
	if (Name.empty() || Name.find('/') != std::string::npos || Name.find("..") != std::string::npos) {
		Machine.Fail(Line, "invalid package name", true);
	}
	fs::path Dir = fs::path(Machine.Config().DatabaseDirectory) / Name;
	std::error_code Error;
	fs::create_directories(Dir, Error);
	if (Error) Machine.Fail(Line, "cannot create package record", true);
	{
		std::ofstream VersionFile(Dir / "version");
		if (!VersionFile) Machine.Fail(Line, "cannot write package version", true);
		VersionFile << Version << "\n";
	}
	{
		std::time_t Now = std::time(nullptr);
		std::tm Utc{};
		gmtime_r(&Now, &Utc);
		char Buffer[32];
		std::strftime(Buffer, sizeof(Buffer), "%Y-%m-%dT%H:%M:%SZ", &Utc);
		std::ofstream TimeFile(Dir / "installed_time");
		if (!TimeFile) Machine.Fail(Line, "cannot write install time", true);
		TimeFile << Buffer << "\n";
	}
	fs::path ManifestPath = ResolveSource(Machine, Manifest);
	if (fs::is_regular_file(ManifestPath)) {
		fs::copy_file(ManifestPath, Dir / "manifest", fs::copy_options::overwrite_existing, Error);
		if (Error) Machine.Fail(Line, "cannot copy manifest", true);
	}
	LogLine("\033[1;32m", "[OPSIS:INFO]",
		"Recorded package " + Name + "-" + Version + " in system database.", std::cout);
}

void AddNative(Environment &Globals, Module &Mod, const std::string &Name, int MinArgs, int MaxArgs,
	const std::function<Value(Interpreter &, const std::vector<Value> &)> &Fn)
{
	Value Item = Value::FromNative(Name, MinArgs, MaxArgs, Fn);
	Globals.Define(Name, Item);
	Mod.Members[Name] = Item;
}

} // namespace

Interpreter::Interpreter(RuntimeConfig Config, std::string FileName)
	: Config_(std::move(Config)), FileName_(std::move(FileName)), Globals_(nullptr), Current_(&Globals_)
{
	if (!Config_.BaseDirectory.empty()) ScriptDirectory_ = fs::absolute(Config_.BaseDirectory);
	else if (Config_.ScriptPath.empty()) ScriptDirectory_ = fs::current_path();
	else ScriptDirectory_ = fs::absolute(Config_.ScriptPath).parent_path();
	OpsisModule_ = std::make_shared<Module>();

	AddNative(Globals_, *OpsisModule_, "LogInfo", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		LogLine("\033[1;32m", "[OPSIS:INFO]", NeedString(Machine, Args[0], 0), std::cout);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "LogWarn", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		LogLine("\033[1;33m", "[OPSIS:WARN]", NeedString(Machine, Args[0], 0), std::cerr);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "LogError", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		LogLine("\033[1;31m", "[OPSIS:FAIL]", NeedString(Machine, Args[0], 0), std::cerr);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "LogStep", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		LogLine("\033[1;36m", "[OPSIS:STEP]", NeedString(Machine, Args[0], 0), std::cout);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "Fail", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		Machine.Fail(0, NeedString(Machine, Args[0], 0), true);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "CheckUser", 0, 0, [](Interpreter &Machine, const std::vector<Value> &) {
		NativeCheckUser(Machine, 0);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "IsRoot", 0, 0, [](Interpreter &, const std::vector<Value> &) {
		return Value::FromBool(geteuid() == 0);
	});
	AddNative(Globals_, *OpsisModule_, "DiskFreeKb", 0, 0, [](Interpreter &Machine, const std::vector<Value> &) {
		struct statvfs Stat {};
		if (statvfs(Machine.Config().Sysroot.c_str(), &Stat) != 0) return Value::FromInt(-1);
		return Value::FromInt(static_cast<int64_t>(Stat.f_bavail) * static_cast<int64_t>(Stat.f_frsize) / 1024);
	});
	AddNative(Globals_, *OpsisModule_, "CheckDiskSpace", 1, 1,
		[](Interpreter &Machine, const std::vector<Value> &Args) {
		int64_t Need = NeedInt(Machine, Args[0], 0);
		if (Need <= 0) return Value::Null();
		struct statvfs Stat {};
		if (statvfs(Machine.Config().Sysroot.c_str(), &Stat) != 0) return Value::Null();
		int64_t Avail = static_cast<int64_t>(Stat.f_bavail) * static_cast<int64_t>(Stat.f_frsize) / 1024;
		if (Avail < Need) {
			Machine.Fail(0, "Insufficient disk space in " + Machine.Config().Sysroot
				+ ": required " + std::to_string(Need) + "KB, available " + std::to_string(Avail) + "KB", true);
		}
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "InstallFile", 2, 4, [](Interpreter &Machine, const std::vector<Value> &Args) {
		NativeInstallFile(Machine, Args, 0);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "InstallDirectory", 1, 2,
		[](Interpreter &Machine, const std::vector<Value> &Args) {
		NativeInstallDirectory(Machine, Args, 0);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "UpdateLdconfig", 0, 0, [](Interpreter &Machine, const std::vector<Value> &) {
		fs::path Root = Machine.Config().Sysroot;
		bool Has = access((Root / "usr/bin/ldconfig").c_str(), X_OK) == 0
			|| access((Root / "sbin/ldconfig").c_str(), X_OK) == 0;
		if (!Has) return Value::Null();
		LogLine("\033[1;36m", "[OPSIS:STEP]", "Updating dynamic linker run-time bindings (ldconfig)...", std::cout);
		if (RunProcess({"chroot", Root.string(), "/usr/bin/ldconfig"}, true) != 0) {
			RunProcess({"ldconfig", "-r", Root.string()}, true);
		}
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "UpdateSystemd", 0, 0, [](Interpreter &Machine, const std::vector<Value> &) {
		fs::path Units = fs::path(Machine.Config().Sysroot) / "run/systemd/system";
		if (!fs::is_directory(Units)) return Value::Null();
		if (std::system("command -v systemctl >/dev/null 2>&1") != 0) return Value::Null();
		LogLine("\033[1;36m", "[OPSIS:STEP]", "Reloading systemd daemon...", std::cout);
		RunProcess({"systemctl", "daemon-reload"}, true);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "RecordInstalled", 3, 3,
		[](Interpreter &Machine, const std::vector<Value> &Args) {
		NativeRecord(Machine, Args, 0);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "Sysroot", 0, 0, [](Interpreter &Machine, const std::vector<Value> &) {
		return Value::FromString(Machine.Config().Sysroot);
	});
	AddNative(Globals_, *OpsisModule_, "ScriptDirectory", 0, 0, [](Interpreter &Machine, const std::vector<Value> &) {
		return Value::FromString(Machine.ScriptDirectory().string());
	});
	Globals_.Define("Opsis", Value::FromModule(OpsisModule_));
}

void Interpreter::DefineFunction(const FunctionStmt *Node, const std::string &ClassName)
{
	auto Fn = std::make_shared<Function>();
	Fn->Name = ClassName.empty() ? Node->Name() : ClassName + "." + Node->Name();
	Fn->ReturnType = Node->ReturnType();
	Fn->Params = Node->Params();
	Fn->Body = Node->Body();
	int Rank = 100;
	if (Node->Name() == "Install") Rank = ClassName.empty() ? 1 : 3;
	else if (Node->Name() == "Main") Rank = ClassName.empty() ? 2 : 4;
	if (Rank < EntryRank_) {
		Entry_ = Fn;
		EntryRank_ = Rank;
	}
	if (ClassName.empty()) Globals_.Define(Node->Name(), Value::FromFunction(Fn));
}

void Interpreter::Run(const std::vector<StmtPtr> &Items)
{
	std::vector<const Stmt *> Top;
	for (const auto &Item : Items) {
		if (auto *Fn = dynamic_cast<const FunctionStmt *>(Item.get())) {
			DefineFunction(Fn, "");
		} else if (auto *Cls = dynamic_cast<const ClassStmt *>(Item.get())) {
			auto Mod = std::make_shared<Module>();
			for (const auto &Method : Cls->Methods()) {
				DefineFunction(Method.get(), Cls->Name());
				auto Fn = std::make_shared<Function>();
				Fn->Name = Cls->Name() + "." + Method->Name();
				Fn->ReturnType = Method->ReturnType();
				Fn->Params = Method->Params();
				Fn->Body = Method->Body();
				Mod->Members[Method->Name()] = Value::FromFunction(Fn);
			}
			Globals_.Define(Cls->Name(), Value::FromModule(Mod));
		} else {
			Top.push_back(Item.get());
		}
	}
	if (!Top.empty()) {
		for (const Stmt *Item : Top) Item->Execute(*this);
		return;
	}
	if (!Entry_) Fail(1, "no Install or Main method", false);
	CallFunction(*Entry_, {}, Entry_ ? 1 : 1);
}

std::vector<StmtPtr> ParseSource(const std::string &Source, const std::string &FileName)
{
	Parser Tokens(Source, FileName);
	return Tokens.Parse();
}

} // namespace

RuntimeConfig LoadConfig()
{
	RuntimeConfig Config;
	const char *Sysroot = std::getenv("OPSIS_SYSROOT");
	Config.Sysroot = Sysroot && *Sysroot ? Sysroot : "/";
	const char *Allow = std::getenv("OPSIS_ALLOW_NONROOT");
	Config.AllowNonRoot = Allow && std::string(Allow) == "1";
	const char *Database = std::getenv("OPSIS_DB_DIR");
	if (Database && *Database) Config.DatabaseDirectory = Database;
	else Config.DatabaseDirectory = (fs::path(Config.Sysroot) / "var/lib/okrapm/db").string();
	return Config;
}

int RunSource(const std::string &Source, const std::string &FileName, const RuntimeConfig &Config)
{
	try {
		if (!fs::is_directory(Config.Sysroot)) {
			std::cerr << "\033[1;31m[OPSIS:FAIL]\033[0m Target sysroot does not exist: " << Config.Sysroot << "\n";
			return 1;
		}
		std::error_code Error;
		fs::create_directories(Config.DatabaseDirectory, Error);
		if (Error) {
			std::cerr << "\033[1;31m[OPSIS:FAIL]\033[0m Cannot create database directory: "
				<< Config.DatabaseDirectory << "\n";
			return 1;
		}
		auto Items = ParseSource(Source, FileName);
		Interpreter Machine(Config, FileName);
		Machine.Run(Items);
		return 0;
	} catch (const ReturnSignal &) {
		return 0;
	} catch (const RuntimeError &Error) {
		if (!Error.Printed) std::cerr << Error.what() << "\n";
		return Error.Code;
	}
}

int RunFile(const std::string &Path, RuntimeConfig Config)
{
	std::ifstream Input(Path);
	if (!Input) {
		std::cerr << "opsis: cannot read " << Path << "\n";
		return 2;
	}
	std::stringstream Buffer;
	Buffer << Input.rdbuf();
	Config.ScriptPath = Path;
	return RunSource(Buffer.str(), Path, Config);
}

int RunPackageScript(const std::string &PackageDir, const std::string &Sysroot, bool AllowNonRoot)
{
	fs::path Root(PackageDir);
	fs::path Script = Root / "scripts/install.opsis";
	if (!fs::is_regular_file(Script)) Script = Root / "install.opsis";
	if (!fs::is_regular_file(Script)) return 0;

	RuntimeConfig Config = LoadConfig();
	Config.Sysroot = Sysroot;
	Config.AllowNonRoot = AllowNonRoot || Config.AllowNonRoot;
	if (geteuid() != 0 && Sysroot != "/") Config.AllowNonRoot = true;
	const char *Database = std::getenv("OPSIS_DB_DIR");
	if (!(Database && *Database)) {
		Config.DatabaseDirectory = (fs::path(Sysroot) / "var/lib/okrapm/db").string();
	}
	Config.BaseDirectory = fs::absolute(Root).string();
	return RunFile(Script.string(), Config);
}

} // namespace Opsis
