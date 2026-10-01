#include "opsis/interpreter.h"

#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
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
#include <unordered_set>
#include <utility>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

namespace Opsis {

const char *HostArchitecture();

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
	Array,
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
	static Value FromArray(std::string ElementType, std::vector<Value> Items);
	static Value FromNative(std::string Name, int MinArgs, int MaxArgs,
		std::function<Value(Interpreter &, const std::vector<Value> &)> Fn);
	static Value FromFunction(std::shared_ptr<Function> Fn);
	static Value FromModule(std::shared_ptr<Module> Mod);

	ValueKind Kind() const { return Kind_; }
	bool IsNull() const { return Kind_ == ValueKind::Null; }
	bool IsInt() const { return Kind_ == ValueKind::Int; }
	bool IsBool() const { return Kind_ == ValueKind::Bool; }
	bool IsString() const { return Kind_ == ValueKind::String; }
	bool IsArray() const { return Kind_ == ValueKind::Array; }
	bool IsNative() const { return Kind_ == ValueKind::Native; }
	bool IsFunction() const { return Kind_ == ValueKind::Function; }
	bool IsModule() const { return Kind_ == ValueKind::Module; }

	int64_t Int() const { return Int_; }
	bool Bool() const { return Bool_; }
	const std::string &Text() const { return Text_; }
	const std::string &ElementType() const { return ElementType_; }
	const std::shared_ptr<std::vector<Value>> &Items() const { return Items_; }
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
	std::string ElementType_;
	std::shared_ptr<std::vector<Value>> Items_;
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

struct BreakSignal {};

struct ContinueSignal {};

struct ThrowSignal {
	std::string TypeName;
	std::string Message;
};

struct GotoCaseSignal {
	bool IsDefault{false};
	Value Label;
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
		if (WritableField(Name)) {
			Fields_->Members[Name] = std::move(Item);
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
		if (Fields_) {
			auto Field = Fields_->Members.find(Name);
			if (Field != Fields_->Members.end()) return Field->second;
		}
		if (Parent_) return Parent_->Get(Name);
		throw RuntimeError(2, "name is not defined: " + Name, false);
	}

	bool Has(const std::string &Name) const
	{
		return Values_.count(Name) != 0;
	}

	void BindFields(Module *Fields) { Fields_ = Fields; }

private:
	bool WritableField(const std::string &Name) const
	{
		if (!Fields_) return false;
		auto Found = Fields_->Members.find(Name);
		if (Found == Fields_->Members.end()) return false;
		return !Found->second.IsFunction() && !Found->second.IsNative();
	}

	Environment *Parent_;
	Module *Fields_{nullptr};
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
	std::string Owner;
	std::string ReturnType;
	std::vector<std::pair<std::string, std::string>> Params;
	const Stmt *Body{nullptr};
};

enum class TokenKind {
	End, Ident, Int, String,
	KwTrue, KwFalse, KwNull, KwVoid, KwInt, KwString, KwBool, KwVar,
	KwIf, KwElse, KwWhile, KwFor, KwForeach, KwIn, KwBreak, KwContinue, KwReturn,
	KwClass, KwPublic, KwStatic, KwPrivate, KwImport,
	KwAbstract, KwVirtual, KwOverride, KwThrow, KwNew,
	KwSwitch, KwCase, KwDefault, KwGoto,
	LParen, RParen, LBrace, RBrace, LBracket, RBracket, Comma, Semicolon, Dot, Colon, Question,
	Assign, PlusAssign, MinusAssign, StarAssign, SlashAssign, PercentAssign,
	PlusPlus, MinusMinus,
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
	const std::string &Name() const { return Name_; }

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

class ArrayExpr : public Expr {
public:
	ArrayExpr(int Line, std::vector<ExprPtr> Items)
		: Expr(Line), Items_(std::move(Items)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	std::vector<ExprPtr> Items_;
};

class IndexExpr : public Expr {
public:
	IndexExpr(int Line, ExprPtr Object, ExprPtr Index)
		: Expr(Line), Object_(std::move(Object)), Index_(std::move(Index)) {}
	Value Evaluate(Interpreter &Machine) const override;
	std::pair<ExprPtr, ExprPtr> Release()
	{
		return {std::move(Object_), std::move(Index_)};
	}

private:
	ExprPtr Object_;
	ExprPtr Index_;
};

class CompoundAssignExpr : public Expr {
public:
	CompoundAssignExpr(int Line, std::string Name, TokenKind Op, ExprPtr ValueExpr)
		: Expr(Line), Name_(std::move(Name)), Op_(Op), ValueExpr_(std::move(ValueExpr)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	std::string Name_;
	TokenKind Op_;
	ExprPtr ValueExpr_;
};

class IndexAssignExpr : public Expr {
public:
	IndexAssignExpr(int Line, ExprPtr Object, ExprPtr Index, TokenKind Op, ExprPtr ValueExpr)
		: Expr(Line), Object_(std::move(Object)), Index_(std::move(Index)), Op_(Op),
		ValueExpr_(std::move(ValueExpr)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	ExprPtr Object_;
	ExprPtr Index_;
	TokenKind Op_;
	ExprPtr ValueExpr_;
};

class UpdateExpr : public Expr {
public:
	UpdateExpr(int Line, std::string Name, bool Prefix, int Delta)
		: Expr(Line), Name_(std::move(Name)), Prefix_(Prefix), Delta_(Delta) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	std::string Name_;
	bool Prefix_;
	int Delta_;
};

class TernaryExpr : public Expr {
public:
	TernaryExpr(int Line, ExprPtr Cond, ExprPtr ThenArm, ExprPtr ElseArm)
		: Expr(Line), Cond_(std::move(Cond)), ThenArm_(std::move(ThenArm)), ElseArm_(std::move(ElseArm)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	ExprPtr Cond_;
	ExprPtr ThenArm_;
	ExprPtr ElseArm_;
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

class BreakStmt : public Stmt {
public:
	explicit BreakStmt(int Line) : Stmt(Line) {}
	void Execute(Interpreter &) const override { throw BreakSignal(); }
};

class ContinueStmt : public Stmt {
public:
	explicit ContinueStmt(int Line) : Stmt(Line) {}
	void Execute(Interpreter &) const override { throw ContinueSignal(); }
};

class ForeachStmt : public Stmt {
public:
	ForeachStmt(int Line, std::string TypeName, std::string Name, ExprPtr Collection, StmtPtr Body)
		: Stmt(Line), TypeName_(std::move(TypeName)), Name_(std::move(Name)),
		Collection_(std::move(Collection)), Body_(std::move(Body)) {}
	void Execute(Interpreter &Machine) const override;

private:
	std::string TypeName_;
	std::string Name_;
	ExprPtr Collection_;
	StmtPtr Body_;
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
	void MarkOverride() { Override_ = true; }
	bool Override() const { return Override_; }

private:
	std::string Name_;
	std::string ReturnType_;
	std::vector<std::pair<std::string, std::string>> Params_;
	StmtPtr Body_;
	bool Override_{false};
};

struct ClassField {
	std::string Type;
	std::string Name;
	ExprPtr Init;
};

class ClassStmt : public Stmt {
public:
	ClassStmt(int Line, std::string Name, std::string Base, std::vector<ClassField> Fields,
		std::vector<std::unique_ptr<FunctionStmt>> Methods)
		: Stmt(Line), Name_(std::move(Name)), Base_(std::move(Base)), Fields_(std::move(Fields)),
		Methods_(std::move(Methods)) {}
	void Execute(Interpreter &) const override {}
	const std::string &Name() const { return Name_; }
	const std::string &Base() const { return Base_; }
	const std::vector<ClassField> &Fields() const { return Fields_; }
	const std::vector<std::unique_ptr<FunctionStmt>> &Methods() const { return Methods_; }

private:
	std::string Name_;
	std::string Base_;
	std::vector<ClassField> Fields_;
	std::vector<std::unique_ptr<FunctionStmt>> Methods_;
};

struct SwitchArm {
	bool IsDefault{false};
	ExprPtr Label;
	std::vector<StmtPtr> Body;
};

class SwitchStmt : public Stmt {
public:
	SwitchStmt(int Line, ExprPtr Disc, std::vector<SwitchArm> Arms)
		: Stmt(Line), Disc_(std::move(Disc)), Arms_(std::move(Arms)) {}
	void Execute(Interpreter &Machine) const override;

private:
	int FindArm(Interpreter &Machine, const Value &Key, bool WantDefault) const;
	ExprPtr Disc_;
	std::vector<SwitchArm> Arms_;
};

class ThrowStmt : public Stmt {
public:
	ThrowStmt(int Line, std::string TypeName, std::vector<ExprPtr> Args)
		: Stmt(Line), TypeName_(std::move(TypeName)), Args_(std::move(Args)) {}
	void Execute(Interpreter &Machine) const override;

private:
	std::string TypeName_;
	std::vector<ExprPtr> Args_;
};

class GotoCaseStmt : public Stmt {
public:
	GotoCaseStmt(int Line, bool IsDefault, ExprPtr Label)
		: Stmt(Line), IsDefault_(IsDefault), Label_(std::move(Label)) {}
	void Execute(Interpreter &Machine) const override;

private:
	bool IsDefault_;
	ExprPtr Label_;
};

class ImportStmt : public Stmt {
public:
	ImportStmt(int Line, std::string Path) : Stmt(Line), Path_(std::move(Path)) {}
	void Execute(Interpreter &Machine) const override;

private:
	std::string Path_;
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
	~Interpreter();

	void Run(const std::vector<StmtPtr> &Items);
	void DefineFunction(const FunctionStmt *Node, const std::string &ClassName);
	void DefineClass(const ClassStmt *Node);
	void RegisterEntry(const std::shared_ptr<Function> &Fn, int Rank);
	bool DerivesFrom(const std::string &Type, const std::string &Base) const;
	int EntryRankFor(const std::string &MethodName, const std::string &ClassName) const;
	void ImportSource(const std::string &Path, int Line);
	void BindContext();
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
	std::shared_ptr<Module> Context_;
	std::unordered_set<std::string> Imported_;
	std::vector<std::vector<StmtPtr>> OwnedSources_;
	std::unordered_map<std::string, std::string> BaseOf_;
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

Value Value::FromArray(std::string ElementType, std::vector<Value> Items)
{
	Value Item;
	Item.Kind_ = ValueKind::Array;
	Item.ElementType_ = std::move(ElementType);
	Item.Items_ = std::make_shared<std::vector<Value>>(std::move(Items));
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
	case ValueKind::Array: return "array";
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
		if (Ch == '+' && Peek() == '+') { At_ += 2; return Make(TokenKind::PlusPlus, "++"); }
		if (Ch == '-' && Peek() == '-') { At_ += 2; return Make(TokenKind::MinusMinus, "--"); }
		if (Ch == '+' && Peek() == '=') { At_ += 2; return Make(TokenKind::PlusAssign, "+="); }
		if (Ch == '-' && Peek() == '=') { At_ += 2; return Make(TokenKind::MinusAssign, "-="); }
		if (Ch == '*' && Peek() == '=') { At_ += 2; return Make(TokenKind::StarAssign, "*="); }
		if (Ch == '/' && Peek() == '=') { At_ += 2; return Make(TokenKind::SlashAssign, "/="); }
		if (Ch == '%' && Peek() == '=') { At_ += 2; return Make(TokenKind::PercentAssign, "%="); }
		At_++;
		switch (Ch) {
		case '(': return Make(TokenKind::LParen, "(");
		case ')': return Make(TokenKind::RParen, ")");
		case '{': return Make(TokenKind::LBrace, "{");
		case '}': return Make(TokenKind::RBrace, "}");
		case '[': return Make(TokenKind::LBracket, "[");
		case ']': return Make(TokenKind::RBracket, "]");
		case ',': return Make(TokenKind::Comma, ",");
		case ';': return Make(TokenKind::Semicolon, ";");
		case '.': return Make(TokenKind::Dot, ".");
		case ':': return Make(TokenKind::Colon, ":");
		case '?': return Make(TokenKind::Question, "?");
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
			{"null", TokenKind::KwNull},
			{"void", TokenKind::KwVoid}, {"int", TokenKind::KwInt},
			{"string", TokenKind::KwString}, {"bool", TokenKind::KwBool},
			{"var", TokenKind::KwVar}, {"if", TokenKind::KwIf},
			{"else", TokenKind::KwElse}, {"while", TokenKind::KwWhile},
			{"for", TokenKind::KwFor}, {"foreach", TokenKind::KwForeach},
			{"in", TokenKind::KwIn}, {"break", TokenKind::KwBreak},
			{"continue", TokenKind::KwContinue}, {"return", TokenKind::KwReturn},
			{"class", TokenKind::KwClass}, {"public", TokenKind::KwPublic},
			{"static", TokenKind::KwStatic}, {"private", TokenKind::KwPrivate},
			{"import", TokenKind::KwImport}, {"abstract", TokenKind::KwAbstract},
			{"virtual", TokenKind::KwVirtual}, {"override", TokenKind::KwOverride},
			{"throw", TokenKind::KwThrow}, {"new", TokenKind::KwNew},
			{"switch", TokenKind::KwSwitch}, {"case", TokenKind::KwCase},
			{"default", TokenKind::KwDefault}, {"goto", TokenKind::KwGoto}
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
		Advance();
		Advance();
	}

	std::vector<StmtPtr> Parse()
	{
		std::vector<StmtPtr> Items;
		while (!Check(TokenKind::End)) {
			SkipModifiers();
			if (Check(TokenKind::KwClass)) Items.push_back(ParseClass());
			else if (LooksLikeFunction()) Items.push_back(ParseFunction());
			else Items.push_back(ParseStatement());
		}
		return Items;
	}

private:
	void Advance()
	{
		Current_ = Upcoming_;
		Upcoming_ = Third_;
		Third_ = Fourth_;
		Fourth_ = Fifth_;
		Fifth_ = Lexer_.Next();
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
		while (Check(TokenKind::KwPublic) || Check(TokenKind::KwStatic) || Check(TokenKind::KwPrivate)
			|| Check(TokenKind::KwAbstract)) {
			Advance();
		}
	}

	bool TakeMemberModifiers()
	{
		bool Override = false;
		for (;;) {
			if (Check(TokenKind::KwPublic) || Check(TokenKind::KwStatic) || Check(TokenKind::KwPrivate)
				|| Check(TokenKind::KwVirtual) || Check(TokenKind::KwAbstract)) {
				Advance();
			} else if (Check(TokenKind::KwOverride)) {
				Override = true;
				Advance();
			} else {
				break;
			}
		}
		return Override;
	}

	bool IsType(TokenKind Kind) const
	{
		return Kind == TokenKind::KwVoid || Kind == TokenKind::KwInt || Kind == TokenKind::KwString
			|| Kind == TokenKind::KwBool || Kind == TokenKind::KwVar;
	}

	bool LooksLikeFunction() const
	{
		if (Current_.Kind == TokenKind::Ident) {
			return Upcoming_.Kind == TokenKind::Ident && Third_.Kind == TokenKind::LParen;
		}
		if (!IsType(Current_.Kind)) return false;
		if (Upcoming_.Kind == TokenKind::Ident && Third_.Kind == TokenKind::LParen) return true;
		return Upcoming_.Kind == TokenKind::LBracket && Third_.Kind == TokenKind::RBracket
			&& Fourth_.Kind == TokenKind::Ident && Fifth_.Kind == TokenKind::LParen;
	}

	bool IsAssignOp(TokenKind Kind) const
	{
		return Kind == TokenKind::Assign || Kind == TokenKind::PlusAssign || Kind == TokenKind::MinusAssign
			|| Kind == TokenKind::StarAssign || Kind == TokenKind::SlashAssign || Kind == TokenKind::PercentAssign;
	}

	std::string ParseTypeName(bool AllowVoid, bool AllowVar)
	{
		if (Check(TokenKind::Ident)) {
			std::string Name = Current_.Text;
			Advance();
			return Name;
		}
		if (!IsType(Current_.Kind)) throw RuntimeError(2, Where() + "expected a type", false);
		if (!AllowVoid && Current_.Kind == TokenKind::KwVoid) {
			throw RuntimeError(2, Where() + "expected a type", false);
		}
		if (!AllowVar && Current_.Kind == TokenKind::KwVar) {
			throw RuntimeError(2, Where() + "expected a type", false);
		}
		std::string Name = TypeText(Current_.Kind);
		Advance();
		if (!Match(TokenKind::LBracket)) return Name;
		if (Name == "void" || Name == "var") throw RuntimeError(2, Where() + "this type cannot be an array", false);
		Expect(TokenKind::RBracket, "']'");
		return Name + "[]";
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
		std::string Base;
		if (Match(TokenKind::Colon)) {
			if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected a base class", false);
			Base = Current_.Text;
			Advance();
		}
		Expect(TokenKind::LBrace, "'{'");
		std::vector<ClassField> Fields;
		std::vector<std::unique_ptr<FunctionStmt>> Methods;
		while (!Check(TokenKind::RBrace) && !Check(TokenKind::End)) {
			bool Override = TakeMemberModifiers();
			if (!IsType(Current_.Kind) && Current_.Kind != TokenKind::Ident) {
				throw RuntimeError(2, Where() + "expected a field or method", false);
			}
			if (LooksLikeFunction()) {
				auto Node = ParseFunction();
				auto *Fn = dynamic_cast<FunctionStmt *>(Node.get());
				if (!Fn) throw RuntimeError(2, Where() + "expected a method", false);
				if (Override) Fn->MarkOverride();
				Node.release();
				Methods.emplace_back(Fn);
				continue;
			}
			if (Override) throw RuntimeError(2, Where() + "override applies to a method", false);
			ClassField Field;
			Field.Type = ParseTypeName(false, true);
			if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected a field name", false);
			Field.Name = Current_.Text;
			Advance();
			if (Match(TokenKind::Assign)) Field.Init = ParseExpression();
			Expect(TokenKind::Semicolon, "';'");
			Fields.push_back(std::move(Field));
		}
		Expect(TokenKind::RBrace, "'}'");
		return std::make_unique<ClassStmt>(Line, Name, Base, std::move(Fields), std::move(Methods));
	}

	StmtPtr ParseFunction()
	{
		int Line = Current_.Line;
		std::string ReturnType = ParseTypeName(true, true);
		if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected a method name", false);
		std::string Name = Current_.Text;
		Advance();
		Expect(TokenKind::LParen, "'('");
		std::vector<std::pair<std::string, std::string>> Params;
		if (!Check(TokenKind::RParen)) {
			do {
				std::string ParamType = ParseTypeName(false, false);
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

	StmtPtr ParseImport()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwImport, "import");
		if (!Check(TokenKind::String)) throw RuntimeError(2, Where() + "expected an import path", false);
		std::string Path = Current_.Text;
		Advance();
		Expect(TokenKind::Semicolon, "';'");
		return std::make_unique<ImportStmt>(Line, Path);
	}

	StmtPtr ParseThrow()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwThrow, "throw");
		Expect(TokenKind::KwNew, "new");
		if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected an exception type", false);
		std::string TypeName = Current_.Text;
		Advance();
		Expect(TokenKind::LParen, "'('");
		std::vector<ExprPtr> Args;
		if (!Check(TokenKind::RParen)) {
			do {
				Args.push_back(ParseExpression());
			} while (Match(TokenKind::Comma));
		}
		Expect(TokenKind::RParen, "')'");
		Expect(TokenKind::Semicolon, "';'");
		return std::make_unique<ThrowStmt>(Line, TypeName, std::move(Args));
	}

	StmtPtr ParseGotoCase()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwGoto, "goto");
		if (Match(TokenKind::KwDefault)) {
			Expect(TokenKind::Semicolon, "';'");
			return std::make_unique<GotoCaseStmt>(Line, true, nullptr);
		}
		Expect(TokenKind::KwCase, "case");
		ExprPtr Label = ParseExpression();
		Expect(TokenKind::Semicolon, "';'");
		return std::make_unique<GotoCaseStmt>(Line, false, std::move(Label));
	}

	StmtPtr ParseSwitch()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwSwitch, "switch");
		Expect(TokenKind::LParen, "'('");
		ExprPtr Disc = ParseExpression();
		Expect(TokenKind::RParen, "')'");
		Expect(TokenKind::LBrace, "'{'");
		std::vector<SwitchArm> Arms;
		while (!Check(TokenKind::RBrace) && !Check(TokenKind::End)) {
			SwitchArm Arm;
			if (Match(TokenKind::KwDefault)) {
				Arm.IsDefault = true;
			} else {
				Expect(TokenKind::KwCase, "case");
				Arm.Label = ParseExpression();
			}
			Expect(TokenKind::Colon, "':'");
			while (!Check(TokenKind::RBrace) && !Check(TokenKind::KwCase) && !Check(TokenKind::KwDefault)
				&& !Check(TokenKind::End)) {
				Arm.Body.push_back(ParseStatement());
			}
			Arms.push_back(std::move(Arm));
		}
		Expect(TokenKind::RBrace, "'}'");
		return std::make_unique<SwitchStmt>(Line, std::move(Disc), std::move(Arms));
	}

	StmtPtr ParseStatement()
	{
		SkipModifiers();
		if (Check(TokenKind::KwImport)) return ParseImport();
		if (Check(TokenKind::LBrace)) return ParseBlock();
		if (Check(TokenKind::KwIf)) return ParseIf();
		if (Check(TokenKind::KwWhile)) return ParseWhile();
		if (Check(TokenKind::KwFor)) return ParseFor();
		if (Check(TokenKind::KwForeach)) return ParseForeach();
		if (Check(TokenKind::KwBreak)) return ParseBreak();
		if (Check(TokenKind::KwContinue)) return ParseContinue();
		if (Check(TokenKind::KwReturn)) return ParseReturn();
		if (Check(TokenKind::KwThrow)) return ParseThrow();
		if (Check(TokenKind::KwSwitch)) return ParseSwitch();
		if (Check(TokenKind::KwGoto)) return ParseGotoCase();
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

	StmtPtr ParseForeach()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwForeach, "foreach");
		Expect(TokenKind::LParen, "'('");
		std::string TypeName = ParseTypeName(false, true);
		if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected a variable name", false);
		std::string Name = Current_.Text;
		Advance();
		Expect(TokenKind::KwIn, "in");
		ExprPtr Collection = ParseExpression();
		Expect(TokenKind::RParen, "')'");
		return std::make_unique<ForeachStmt>(Line, TypeName, Name, std::move(Collection), ParseStatement());
	}

	StmtPtr ParseBreak()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwBreak, "break");
		Expect(TokenKind::Semicolon, "';'");
		return std::make_unique<BreakStmt>(Line);
	}

	StmtPtr ParseContinue()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwContinue, "continue");
		Expect(TokenKind::Semicolon, "';'");
		return std::make_unique<ContinueStmt>(Line);
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
		std::string TypeName = ParseTypeName(false, true);
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
		ExprPtr Left = ParseTernary();
		if (!IsAssignOp(Current_.Kind)) return Left;
		TokenKind Op = Current_.Kind;
		int Line = Current_.Line;
		Advance();
		ExprPtr Right = ParseAssignment();
		if (auto *Variable = dynamic_cast<VariableExpr *>(Left.get())) {
			if (Op == TokenKind::Assign) {
				return std::make_unique<AssignExpr>(Line, Variable->Name(), std::move(Right));
			}
			return std::make_unique<CompoundAssignExpr>(Line, Variable->Name(), Op, std::move(Right));
		}
		if (auto *Index = dynamic_cast<IndexExpr *>(Left.get())) {
			auto Parts = Index->Release();
			return std::make_unique<IndexAssignExpr>(Line, std::move(Parts.first), std::move(Parts.second), Op,
				std::move(Right));
		}
		throw RuntimeError(2, Where() + "expected an assignable value", false);
	}

	ExprPtr ParseTernary()
	{
		ExprPtr Cond = ParseOr();
		if (!Match(TokenKind::Question)) return Cond;
		int Line = Cond->Line();
		ExprPtr ThenArm = ParseExpression();
		Expect(TokenKind::Colon, "':'");
		return std::make_unique<TernaryExpr>(Line, std::move(Cond), std::move(ThenArm), ParseTernary());
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
		if (Current_.Kind == TokenKind::PlusPlus || Current_.Kind == TokenKind::MinusMinus) {
			int Delta = Current_.Kind == TokenKind::PlusPlus ? 1 : -1;
			int Line = Current_.Line;
			Advance();
			ExprPtr Right = ParseUnary();
			auto *Variable = dynamic_cast<VariableExpr *>(Right.get());
			if (!Variable) throw RuntimeError(2, Where() + "expected a variable", false);
			return std::make_unique<UpdateExpr>(Line, Variable->Name(), true, Delta);
		}
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
			} else if (Match(TokenKind::LBracket)) {
				int Line = Item->Line();
				ExprPtr Index = ParseExpression();
				Expect(TokenKind::RBracket, "']'");
				Item = std::make_unique<IndexExpr>(Line, std::move(Item), std::move(Index));
			} else if (Current_.Kind == TokenKind::PlusPlus || Current_.Kind == TokenKind::MinusMinus) {
				auto *Variable = dynamic_cast<VariableExpr *>(Item.get());
				if (!Variable) throw RuntimeError(2, Where() + "expected a variable", false);
				int Delta = Current_.Kind == TokenKind::PlusPlus ? 1 : -1;
				int Line = Current_.Line;
				std::string Name = Variable->Name();
				Advance();
				Item = std::make_unique<UpdateExpr>(Line, Name, false, Delta);
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
		if (Match(TokenKind::KwNull)) return std::make_unique<LiteralExpr>(Line, Value::Null());
		if (Match(TokenKind::LBrace)) return ParseArray(Line);
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

	ExprPtr ParseArray(int Line)
	{
		std::vector<ExprPtr> Items;
		if (!Check(TokenKind::RBrace)) {
			do {
				Items.push_back(ParseExpression());
			} while (Match(TokenKind::Comma) && !Check(TokenKind::RBrace));
		}
		Expect(TokenKind::RBrace, "'}'");
		return std::make_unique<ArrayExpr>(Line, std::move(Items));
	}

	Lexer Lexer_;
	std::string FileName_;
	Token Current_;
	Token Upcoming_;
	Token Third_;
	Token Fourth_;
	Token Fifth_;
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

bool IsArrayTypeName(const std::string &TypeName)
{
	return TypeName.size() > 2 && TypeName.compare(TypeName.size() - 2, 2, "[]") == 0;
}

std::string ArrayElementName(const std::string &TypeName)
{
	if (!IsArrayTypeName(TypeName)) return "";
	return TypeName.substr(0, TypeName.size() - 2);
}

bool TypeOk(const std::string &TypeName, const Value &Item)
{
	if (TypeName == "var") return true;
	if (TypeName == "int") return Item.IsInt();
	if (TypeName == "string") return Item.IsString();
	if (TypeName == "bool") return Item.IsBool();
	if (TypeName == "void") return Item.IsNull();
	if (IsArrayTypeName(TypeName)) {
		return Item.IsArray() && Item.ElementType() == ArrayElementName(TypeName);
	}
	return Item.IsModule();
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
	if (Op_ == TokenKind::EqualEqual || Op_ == TokenKind::BangEqual) {
		if (Left.IsNull() || Right.IsNull()) {
			bool Same = Left.IsNull() && Right.IsNull();
			return Value::FromBool(Op_ == TokenKind::EqualEqual ? Same : !Same);
		}
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

Value Compute(Interpreter &Machine, int Line, Value Left, TokenKind Op, Value Right)
{
	auto LeftExpr = std::make_unique<LiteralExpr>(Line, std::move(Left));
	auto RightExpr = std::make_unique<LiteralExpr>(Line, std::move(Right));
	BinaryExpr Node(Line, std::move(LeftExpr), Op, std::move(RightExpr));
	return Node.Evaluate(Machine);
}

TokenKind PlainOp(TokenKind Op)
{
	if (Op == TokenKind::PlusAssign) return TokenKind::Plus;
	if (Op == TokenKind::MinusAssign) return TokenKind::Minus;
	if (Op == TokenKind::StarAssign) return TokenKind::Star;
	if (Op == TokenKind::SlashAssign) return TokenKind::Slash;
	if (Op == TokenKind::PercentAssign) return TokenKind::Percent;
	return Op;
}

Value ArrayExpr::Evaluate(Interpreter &Machine) const
{
	std::vector<Value> Items;
	std::string ElementType;
	for (const auto &ItemExpr : Items_) {
		Value Item = ItemExpr->Evaluate(Machine);
		std::string This;
		if (Item.IsInt()) This = "int";
		else if (Item.IsString()) This = "string";
		else if (Item.IsBool()) This = "bool";
		else Machine.Fail(Line(), "array elements must be int, string, or bool", false);
		if (ElementType.empty()) ElementType = This;
		else if (ElementType != This) Machine.Fail(Line(), "array element type mismatch", false);
		Items.push_back(std::move(Item));
	}
	return Value::FromArray(ElementType, std::move(Items));
}

Value IndexExpr::Evaluate(Interpreter &Machine) const
{
	Value Object = Object_->Evaluate(Machine);
	if (!Object.IsArray()) Machine.Fail(Line(), "value is not an array", false);
	int64_t At = NeedInt(Machine, Index_->Evaluate(Machine), Line());
	if (At < 0 || static_cast<uint64_t>(At) >= Object.Items()->size()) {
		Machine.Fail(Line(), "array index out of range", false);
	}
	return (*Object.Items())[static_cast<size_t>(At)];
}

Value CompoundAssignExpr::Evaluate(Interpreter &Machine) const
{
	Value Current;
	try {
		Current = Machine.Current()->Get(Name_);
	} catch (const RuntimeError &Error) {
		Machine.Fail(Line(), Error.what(), false);
	}
	Value Next = Compute(Machine, Line(), Current, PlainOp(Op_), ValueExpr_->Evaluate(Machine));
	try {
		Machine.Current()->Assign(Name_, Next);
	} catch (const RuntimeError &Error) {
		Machine.Fail(Line(), Error.what(), false);
	}
	return Next;
}

Value IndexAssignExpr::Evaluate(Interpreter &Machine) const
{
	Value Object = Object_->Evaluate(Machine);
	if (!Object.IsArray()) Machine.Fail(Line(), "value is not an array", false);
	int64_t At = NeedInt(Machine, Index_->Evaluate(Machine), Line());
	if (At < 0 || static_cast<uint64_t>(At) >= Object.Items()->size()) {
		Machine.Fail(Line(), "array index out of range", false);
	}
	Value Next = ValueExpr_->Evaluate(Machine);
	if (Op_ != TokenKind::Assign) {
		Next = Compute(Machine, Line(), (*Object.Items())[static_cast<size_t>(At)], PlainOp(Op_), Next);
	}
	if (!Object.ElementType().empty() && !TypeOk(Object.ElementType(), Next)) {
		Machine.Fail(Line(), "array element type mismatch", false);
	}
	(*Object.Items())[static_cast<size_t>(At)] = Next;
	return Next;
}

Value UpdateExpr::Evaluate(Interpreter &Machine) const
{
	Value Current;
	try {
		Current = Machine.Current()->Get(Name_);
	} catch (const RuntimeError &Error) {
		Machine.Fail(Line(), Error.what(), false);
	}
	TokenKind Op = Delta_ < 0 ? TokenKind::Minus : TokenKind::Plus;
	Value Next = Compute(Machine, Line(), Current, Op, Value::FromInt(1));
	try {
		Machine.Current()->Assign(Name_, Next);
	} catch (const RuntimeError &Error) {
		Machine.Fail(Line(), Error.what(), false);
	}
	return Prefix_ ? Next : Current;
}

Value TernaryExpr::Evaluate(Interpreter &Machine) const
{
	if (NeedBool(Machine, Cond_->Evaluate(Machine), Line())) return ThenArm_->Evaluate(Machine);
	return ElseArm_->Evaluate(Machine);
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

int64_t VersionPart(Interpreter &Machine, const std::string &Text, const std::string &Name, int Line);

Value GetExpr::Evaluate(Interpreter &Machine) const
{
	Value Object = Object_->Evaluate(Machine);
	if (Name_ == "Length" && Object.IsString()) return Value::FromInt(static_cast<int64_t>(Object.Text().size()));
	if (Name_ == "Length" && Object.IsArray()) {
		return Value::FromInt(static_cast<int64_t>(Object.Items()->size()));
	}
	if (Object.IsString() && (Name_ == "Major" || Name_ == "Minor" || Name_ == "Patch")) {
		return Value::FromInt(VersionPart(Machine, Object.Text(), Name_, Line()));
	}
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
	else if (IsArrayTypeName(TypeName_)) Item = Value::FromArray(ArrayElementName(TypeName_), {});
	if (Init_ && Item.IsArray() && Item.ElementType().empty() && IsArrayTypeName(TypeName_)) {
		Item = Value::FromArray(ArrayElementName(TypeName_), *Item.Items());
	}
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
	while (NeedBool(Machine, Cond_->Evaluate(Machine), Line())) {
		try {
			Body_->Execute(Machine);
		} catch (const ContinueSignal &) {
		} catch (const BreakSignal &) {
			break;
		}
	}
}

void ForStmt::Execute(Interpreter &Machine) const
{
	Environment Local(Machine.Current());
	EnvironmentGuard Guard(Machine, &Local);
	Init_->Execute(Machine);
	while (NeedBool(Machine, Cond_->Evaluate(Machine), Line())) {
		try {
			Body_->Execute(Machine);
		} catch (const ContinueSignal &) {
		} catch (const BreakSignal &) {
			break;
		}
		if (Step_) Step_->Evaluate(Machine);
	}
}

void ReturnStmt::Execute(Interpreter &Machine) const
{
	Value Item = Value::Null();
	if (ValueExpr_) Item = ValueExpr_->Evaluate(Machine);
	throw ReturnSignal{Item};
}

void ForeachStmt::Execute(Interpreter &Machine) const
{
	Value Collection = Collection_->Evaluate(Machine);
	if (!Collection.IsArray()) Machine.Fail(Line(), "foreach needs an array", false);
	if (TypeName_ != "var" && !Collection.ElementType().empty() && Collection.ElementType() != TypeName_) {
		Machine.Fail(Line(), "foreach element type mismatch", false);
	}
	std::vector<Value> Items = *Collection.Items();
	for (const Value &Item : Items) {
		if (!TypeOk(TypeName_, Item)) Machine.Fail(Line(), "foreach element type mismatch", false);
		Environment Local(Machine.Current());
		EnvironmentGuard Guard(Machine, &Local);
		Local.Define(Name_, Item);
		try {
			Body_->Execute(Machine);
		} catch (const ContinueSignal &) {
		} catch (const BreakSignal &) {
			break;
		}
	}
}

int64_t VersionPart(Interpreter &Machine, const std::string &Text, const std::string &Name, int Line)
{
	std::string Core = Text;
	auto Dash = Core.find('-');
	if (Dash != std::string::npos) Core = Core.substr(0, Dash);
	int64_t Parts[3] = {0, 0, 0};
	int Count = 0;
	std::string Token;
	auto Flush = [&]() {
		if (Token.empty() || Count >= 3) Machine.Fail(Line, "value has no version component", false);
		for (char Ch : Token) {
			if (!std::isdigit(static_cast<unsigned char>(Ch))) {
				Machine.Fail(Line, "value has no version component", false);
			}
		}
		try {
			Parts[Count++] = std::stoll(Token);
		} catch (...) {
			Machine.Fail(Line, "value has no version component", false);
		}
		Token.clear();
	};
	for (size_t Index = 0; Index <= Core.size(); ++Index) {
		if (Index == Core.size() || Core[Index] == '.') Flush();
		else Token.push_back(Core[Index]);
	}
	if (Count == 0) Machine.Fail(Line, "value has no version component", false);
	if (Name == "Major") return Parts[0];
	if (Name == "Minor") return Parts[1];
	return Parts[2];
}

bool SameSwitchKey(Interpreter &Machine, int Line, const Value &Left, const Value &Right)
{
	if (Left.IsInt() && Right.IsInt()) return Left.Int() == Right.Int();
	if (Left.IsString() && Right.IsString()) return Left.Text() == Right.Text();
	Machine.Fail(Line, "switch cases must share a type", false);
	return false;
}

int SwitchStmt::FindArm(Interpreter &Machine, const Value &Key, bool WantDefault) const
{
	int DefaultIndex = -1;
	for (int Index = 0; Index < static_cast<int>(Arms_.size()); ++Index) {
		if (Arms_[static_cast<size_t>(Index)].IsDefault) {
			DefaultIndex = Index;
			continue;
		}
		if (WantDefault) continue;
		Value Label = Arms_[static_cast<size_t>(Index)].Label->Evaluate(Machine);
		if (SameSwitchKey(Machine, Line(), Label, Key)) return Index;
	}
	return WantDefault ? DefaultIndex : -1;
}

void SwitchStmt::Execute(Interpreter &Machine) const
{
	Value Key = Disc_->Evaluate(Machine);
	int Index = FindArm(Machine, Key, false);
	if (Index < 0) Index = FindArm(Machine, Key, true);
	while (Index >= 0 && Index < static_cast<int>(Arms_.size())) {
		const SwitchArm &Arm = Arms_[static_cast<size_t>(Index)];
		if (Arm.Body.empty()) {
			++Index;
			continue;
		}
		try {
			for (const auto &Item : Arm.Body) Item->Execute(Machine);
		} catch (const BreakSignal &) {
			return;
		} catch (const GotoCaseSignal &Jump) {
			Index = Jump.IsDefault ? FindArm(Machine, Key, true) : FindArm(Machine, Jump.Label, false);
			if (Index < 0) Machine.Fail(Line(), "switch has no matching case", false);
			continue;
		}
		Machine.Fail(Line(), "switch case falls through", false);
	}
}

void ThrowStmt::Execute(Interpreter &Machine) const
{
	std::string Message;
	for (const auto &Arg : Args_) {
		if (!Message.empty()) Message += ", ";
		Message += Arg->Evaluate(Machine).ToString();
	}
	throw ThrowSignal{TypeName_, Message};
}

void GotoCaseStmt::Execute(Interpreter &Machine) const
{
	if (IsDefault_) throw GotoCaseSignal{true, Value::Null()};
	throw GotoCaseSignal{false, Label_->Evaluate(Machine)};
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
	if (!Fn.Owner.empty()) {
		try {
			Value Owner = Globals_.Get(Fn.Owner);
			if (Owner.IsModule()) Local.BindFields(Owner.ModulePtr().get());
		} catch (const RuntimeError &) {
		}
	}
	for (size_t Index = 0; Index < Fn.Params.size(); ++Index) {
		if (!TypeOk(Fn.Params[Index].first, Args[Index])) {
			Fail(Line, "parameter type mismatch for " + Fn.Params[Index].second, false);
		}
		Local.Define(Fn.Params[Index].second, Args[Index]);
	}
	try {
		Fn.Body->Execute(*this);
	} catch (const BreakSignal &) {
		Fail(Line, "break is outside a loop", false);
	} catch (const ContinueSignal &) {
		Fail(Line, "continue is outside a loop", false);
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

bool ValidPackageName(const std::string &Name)
{
	return !Name.empty() && Name.find('/') == std::string::npos && Name.find("..") == std::string::npos;
}

void ApplyMode(const fs::path &Path, const std::string &Mode)
{
	char *End = nullptr;
	unsigned long Bits = std::strtoul(Mode.c_str(), &End, 8);
	if (End && *End == '\0' && Bits <= 07777) chmod(Path.c_str(), static_cast<mode_t>(Bits));
}

void WriteText(Interpreter &Machine, const std::string &Destination, const std::string &Text, bool Append, int Line)
{
	fs::path To = JoinInside(Machine, Destination, Line);
	std::error_code Error;
	fs::create_directories(To.parent_path(), Error);
	if (Error) Machine.Fail(Line, "cannot create directory " + To.parent_path().string(), true);
	std::ofstream Output(To, Append ? std::ios::app : std::ios::trunc);
	if (!Output) Machine.Fail(Line, "cannot write " + Destination, true);
	Output << Text;
	if (!Output) Machine.Fail(Line, "cannot write " + Destination, true);
	LogLine("\033[1;32m", "[OPSIS:INFO]", "  -> wrote " + Destination, std::cout);
}

bool AllDigits(const std::string &Text)
{
	if (Text.empty()) return false;
	for (unsigned char Ch : Text) {
		if (!std::isdigit(Ch)) return false;
	}
	return true;
}

int CompareVersionText(const std::string &Left, const std::string &Right)
{
	auto Part = [](const std::string &Text) {
		std::vector<std::string> Parts;
		std::string Current;
		for (char Ch : Text) {
			if (Ch == '.') {
				Parts.push_back(Current);
				Current.clear();
			} else {
				Current.push_back(Ch);
			}
		}
		Parts.push_back(Current);
		return Parts;
	};
	std::vector<std::string> A = Part(Left);
	std::vector<std::string> B = Part(Right);
	size_t Count = A.size() > B.size() ? A.size() : B.size();
	for (size_t Index = 0; Index < Count; ++Index) {
		std::string As = Index < A.size() ? A[Index] : "0";
		std::string Bs = Index < B.size() ? B[Index] : "0";
		if (AllDigits(As) && AllDigits(Bs)) {
			try {
				int64_t Ai = std::stoll(As);
				int64_t Bi = std::stoll(Bs);
				if (Ai < Bi) return -1;
				if (Ai > Bi) return 1;
				continue;
			} catch (...) {
			}
		}
		int Cmp = As.compare(Bs);
		if (Cmp < 0) return -1;
		if (Cmp > 0) return 1;
	}
	return 0;
}

bool CommandExists(const std::string &Name)
{
	std::string Probe = "command -v " + Name + " >/dev/null 2>&1";
	return std::system(Probe.c_str()) == 0;
}

void RunHostTool(Interpreter &Machine, const std::string &Tool, const std::string &Relative, const std::string &Step)
{
	fs::path Dir = fs::path(Machine.Config().Sysroot) / Relative;
	if (!fs::is_directory(Dir) || !CommandExists(Tool)) return;
	LogLine("\033[1;36m", "[OPSIS:STEP]", Step, std::cout);
	RunProcess({Tool, Dir.string()}, true);
}

struct DirectPackage {
	bool Open{false};
	std::string Namespace;
	std::string Name;
	std::string Version;
	std::string Description;
	std::string Architecture;
	fs::path Output;
	fs::path Stage;
	std::vector<std::string> Files;
	std::vector<std::string> Dependencies;
	uint64_t InstalledSize{0};
};

DirectPackage PackageSession;

void ResetDirectPackage()
{
	if (!PackageSession.Stage.empty()) {
		std::error_code Error;
		fs::remove_all(PackageSession.Stage, Error);
	}
	PackageSession = {};
}

std::string ConfigOrEnv(const std::string &Field, const char *Name)
{
	if (!Field.empty()) return Field;
	const char *Value = std::getenv(Name);
	if (Value && *Value) return Value;
	return "";
}

bool PackageToken(const std::string &Text)
{
	if (Text.empty() || !std::isalnum(static_cast<unsigned char>(Text[0]))) return false;
	for (unsigned char Ch : Text) {
		if (std::isalnum(Ch) || Ch == '.' || Ch == '_' || Ch == '-' || Ch == '+') continue;
		return false;
	}
	return true;
}

fs::path PackageRelative(Interpreter &Machine, const std::string &Destination, int Line)
{
	fs::path Item(Destination);
	if (!Item.is_absolute()) Machine.Fail(Line, "package path must be absolute: " + Destination, true);
	fs::path Relative = Item.relative_path().lexically_normal();
	if (Relative.empty() || *Relative.begin() == "..") {
		Machine.Fail(Line, "package path escapes the archive: " + Destination, true);
	}
	return Relative;
}

fs::path BuildSource(const Interpreter &Machine, const std::string &Path)
{
	fs::path Item(Path);
	if (Item.is_absolute()) return Item;
	if (!Machine.Config().BuildDirectory.empty()) {
		fs::path Built = fs::path(Machine.Config().BuildDirectory) / Item;
		if (fs::exists(Built)) return Built;
	}
	return Machine.ScriptDirectory() / Item;
}

void RequireOpen(Interpreter &Machine, int Line)
{
	if (!PackageSession.Open) Machine.Fail(Line, "BeginPackage was not called", true);
}

void CopyPackedFile(Interpreter &Machine, const fs::path &From, const fs::path &Relative, const std::string &Mode,
	int Line)
{
	fs::path To = PackageSession.Stage / "rootfs" / Relative;
	std::error_code Error;
	fs::create_directories(To.parent_path(), Error);
	if (Error) Machine.Fail(Line, "cannot create directory " + To.parent_path().string(), true);
	fs::copy_file(From, To, fs::copy_options::overwrite_existing, Error);
	if (Error) Machine.Fail(Line, "cannot pack " + From.string(), true);
	ApplyMode(To, Mode);
	std::string Listed = "/" + Relative.generic_string();
	PackageSession.Files.push_back(Listed);
	PackageSession.InstalledSize += static_cast<uint64_t>(fs::file_size(To, Error));
}

int WriteArchive(const fs::path &Stage, const fs::path &Output)
{
	std::error_code Error;
	fs::remove(Output, Error);
	int Status = RunProcess({"tar", "--zstd", "-cf", Output.string(), "-C", Stage.string(), "."}, true);
	if (Status == 0 && fs::is_regular_file(Output)) return 0;
	fs::remove(Output, Error);
	Status = RunProcess({"tar", "-czf", Output.string(), "-C", Stage.string(), "."}, true);
	if (Status == 0 && fs::is_regular_file(Output)) return 0;
	fs::remove(Output, Error);
	return -1;
}

void WriteChecksum(const fs::path &Output)
{
	if (Output.string().find('\'') != std::string::npos) return;
	std::string Command = "sha256sum '" + Output.string() + "'";
	FILE *Pipe = popen(Command.c_str(), "r");
	if (!Pipe) return;
	char Buffer[128];
	std::string Sum;
	if (std::fgets(Buffer, sizeof(Buffer), Pipe)) Sum = Buffer;
	pclose(Pipe);
	auto Space = Sum.find(' ');
	if (Space == std::string::npos || Space == 0) return;
	std::ofstream Sidecar(Output.string() + ".sha256");
	if (!Sidecar) return;
	Sidecar << Sum.substr(0, Space) << "  " << Output.filename().string() << "\n";
}

void BeginDirectPackage(Interpreter &Machine, const std::vector<Value> &Args, int Line)
{
	if (PackageSession.Open) Machine.Fail(Line, "a package is already open", true);
	std::string Namespace = ConfigOrEnv(Machine.Config().PackageNamespace, "OPSIS_PKG_NAMESPACE");
	std::string Name = ConfigOrEnv(Machine.Config().PackageName, "OPSIS_PKG_NAME");
	std::string Version = ConfigOrEnv(Machine.Config().PackageVersion, "OPSIS_PKG_VERSION");
	std::string Output = ConfigOrEnv(Machine.Config().PackageOutput, "OPSIS_PKG_OUTPUT");
	if (Args.size() >= 3) {
		Namespace = NeedString(Machine, Args[0], Line);
		Name = NeedString(Machine, Args[1], Line);
		Version = NeedString(Machine, Args[2], Line);
	}
	if (Args.size() >= 4) Output = NeedString(Machine, Args[3], Line);
	if (Namespace.empty()) Namespace = "Okra";
	if (!PackageToken(Namespace) || !PackageToken(Name) || !PackageToken(Version)) {
		Machine.Fail(Line, "package name, namespace, or version is empty or unsafe", true);
	}
	if (Output.empty()) Output = Name + "-" + Version + ".oaa";
	fs::path OutputPath = fs::absolute(Output);
	std::string Description = ConfigOrEnv(Machine.Config().PackageDescription, "OPSIS_PKG_DESCRIPTION");
	if (Description.empty()) Description = Name + " direct package";
	std::string Architecture = ConfigOrEnv(Machine.Config().PackageArchitecture, "OPSIS_PKG_ARCH");
	if (Architecture.empty()) Architecture = HostArchitecture();
	fs::path Stage = fs::temp_directory_path() / ("opsis-direct-" + std::to_string(getpid()));
	PackageSession.Stage = Stage;
	std::error_code Error;
	fs::remove_all(Stage, Error);
	fs::create_directories(Stage / "rootfs", Error);
	fs::create_directories(Stage / "scripts", Error);
	if (Error) {
		ResetDirectPackage();
		Machine.Fail(Line, "cannot create package stage", true);
	}
	PackageSession.Open = true;
	PackageSession.Namespace = Namespace;
	PackageSession.Name = Name;
	PackageSession.Version = Version;
	PackageSession.Description = Description;
	PackageSession.Architecture = Architecture;
	PackageSession.Output = OutputPath;
}

void PackOneFile(Interpreter &Machine, const std::vector<Value> &Args, int Line)
{
	RequireOpen(Machine, Line);
	fs::path From = BuildSource(Machine, NeedString(Machine, Args[0], Line));
	if (!fs::is_regular_file(From)) Machine.Fail(Line, "build product does not exist: " + From.string(), true);
	fs::path Relative = PackageRelative(Machine, NeedString(Machine, Args[1], Line), Line);
	std::string Mode = Args.size() >= 3 ? NeedString(Machine, Args[2], Line) : "0755";
	CopyPackedFile(Machine, From, Relative, Mode, Line);
}

void PackOneDirectory(Interpreter &Machine, const std::vector<Value> &Args, int Line)
{
	RequireOpen(Machine, Line);
	fs::path From = BuildSource(Machine, NeedString(Machine, Args[0], Line));
	if (!fs::is_directory(From)) Machine.Fail(Line, "build directory does not exist: " + From.string(), true);
	fs::path Prefix = PackageRelative(Machine, NeedString(Machine, Args[1], Line), Line);
	std::error_code Error;
	for (fs::recursive_directory_iterator It(From, fs::directory_options::skip_permission_denied, Error), End;
		It != End; It.increment(Error)) {
		if (Error) {
			Error.clear();
			continue;
		}
		if (!It->is_regular_file()) continue;
		fs::path Rel = fs::relative(It->path(), From, Error);
		if (Error || Rel.empty() || Rel.generic_string().find("..") != std::string::npos) {
			Machine.Fail(Line, "build path escapes the source directory", true);
		}
		fs::path Destination = Prefix / Rel;
		std::string Mode = (fs::status(It->path()).permissions() & fs::perms::owner_exec) != fs::perms::none
			? "0755" : "0644";
		CopyPackedFile(Machine, It->path(), Destination.lexically_normal(), Mode, Line);
	}
}

void FinishDirectPackage(Interpreter &Machine, const std::vector<Value> &Args, int Line)
{
	RequireOpen(Machine, Line);
	if (!Args.empty()) PackageSession.Output = fs::absolute(NeedString(Machine, Args[0], Line));
	std::string Description = PackageSession.Description;
	for (char &Ch : Description) {
		if (Ch == '"' || Ch == '\n') Ch = ' ';
	}
	{
		std::ofstream Meta(PackageSession.Stage / "meta.yaml");
		if (!Meta) Machine.Fail(Line, "cannot write meta.yaml", true);
		Meta << "name: " << PackageSession.Name << "\n";
		Meta << "namespace: " << PackageSession.Namespace << "\n";
		Meta << "version: " << PackageSession.Version << "\n";
		Meta << "description: \"" << Description << "\"\n";
		Meta << "architecture: " << PackageSession.Architecture << "\n";
		Meta << "maintainer: \"OkraLinux Maintainer <maintainer@okralinux.cn>\"\n";
		Meta << "installed_size: " << PackageSession.InstalledSize << "\n";
		if (!PackageSession.Dependencies.empty()) {
			Meta << "dependencies:\n";
			for (const std::string &Dep : PackageSession.Dependencies) Meta << "  - " << Dep << "\n";
		}
		if (!PackageSession.Files.empty()) {
			Meta << "files:\n";
			for (const std::string &File : PackageSession.Files) Meta << "  - " << File << "\n";
		}
	}
	{
		std::ofstream Script(PackageSession.Stage / "scripts/install.opsis");
		if (!Script) Machine.Fail(Line, "cannot write install.opsis", true);
		Script << "public class Package {\n";
		Script << "    public void INSTALL() {\n";
		Script << "        CheckUser();\n";
		Script << "        InstallDirectory(\"rootfs\", \"/\");\n";
		Script << "        UpdateLdconfig();\n";
		Script << "        RecordInstalled(\"" << PackageSession.Name << "\", \"";
		Script << PackageSession.Version << "\", \"meta.yaml\");\n";
		Script << "    }\n";
		Script << "}\n";
	}
	{
		std::ofstream Script(PackageSession.Stage / "scripts/remove.opsis");
		if (!Script) Machine.Fail(Line, "cannot write remove.opsis", true);
		Script << "public class Package {\n";
		Script << "    public void REMOVE() {\n";
		for (const std::string &File : PackageSession.Files) {
			if (File.find('"') != std::string::npos) continue;
			Script << "        if (FileExists(\"" << File << "\")) {\n";
			Script << "            RemoveFile(\"" << File << "\");\n";
			Script << "        }\n";
		}
		Script << "    }\n";
		Script << "}\n";
	}
	std::error_code Error;
	fs::create_directories(PackageSession.Output.parent_path(), Error);
	if (WriteArchive(PackageSession.Stage, PackageSession.Output) != 0) {
		Machine.Fail(Line, "cannot write " + PackageSession.Output.string(), true);
	}
	WriteChecksum(PackageSession.Output);
	LogLine("\033[1;36m", "[OPSIS:STEP]",
		"Packed " + PackageSession.Namespace + "." + PackageSession.Name + " "
		+ PackageSession.Version + " -> " + PackageSession.Output.string(), std::cout);
	ResetDirectPackage();
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
	AddNative(Globals_, *OpsisModule_, "Length", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		if (Args[0].IsString()) return Value::FromInt(static_cast<int64_t>(Args[0].Text().size()));
		if (Args[0].IsArray()) return Value::FromInt(static_cast<int64_t>(Args[0].Items()->size()));
		Machine.Fail(0, "Length needs a string or an array", false);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "Contains", 2, 2, [](Interpreter &Machine, const std::vector<Value> &Args) {
		const std::string &Text = NeedString(Machine, Args[0], 0);
		const std::string &Part = NeedString(Machine, Args[1], 0);
		return Value::FromBool(Text.find(Part) != std::string::npos);
	});
	AddNative(Globals_, *OpsisModule_, "StartsWith", 2, 2, [](Interpreter &Machine, const std::vector<Value> &Args) {
		const std::string &Text = NeedString(Machine, Args[0], 0);
		const std::string &Part = NeedString(Machine, Args[1], 0);
		return Value::FromBool(Text.size() >= Part.size() && Text.compare(0, Part.size(), Part) == 0);
	});
	AddNative(Globals_, *OpsisModule_, "EndsWith", 2, 2, [](Interpreter &Machine, const std::vector<Value> &Args) {
		const std::string &Text = NeedString(Machine, Args[0], 0);
		const std::string &Part = NeedString(Machine, Args[1], 0);
		if (Text.size() < Part.size()) return Value::FromBool(false);
		return Value::FromBool(Text.compare(Text.size() - Part.size(), Part.size(), Part) == 0);
	});
	AddNative(Globals_, *OpsisModule_, "Substring", 2, 3, [](Interpreter &Machine, const std::vector<Value> &Args) {
		const std::string &Text = NeedString(Machine, Args[0], 0);
		int64_t Start = NeedInt(Machine, Args[1], 0);
		if (Start < 0 || static_cast<uint64_t>(Start) > Text.size()) {
			Machine.Fail(0, "substring is out of range", false);
		}
		int64_t Rest = static_cast<int64_t>(Text.size()) - Start;
		int64_t Count = Args.size() == 3 ? NeedInt(Machine, Args[2], 0) : Rest;
		if (Count < 0 || Count > Rest) Machine.Fail(0, "substring is out of range", false);
		return Value::FromString(Text.substr(static_cast<size_t>(Start), static_cast<size_t>(Count)));
	});
	AddNative(Globals_, *OpsisModule_, "CompareVersion", 2, 2,
		[](Interpreter &Machine, const std::vector<Value> &Args) {
		return Value::FromInt(CompareVersionText(NeedString(Machine, Args[0], 0), NeedString(Machine, Args[1], 0)));
	});
	AddNative(Globals_, *OpsisModule_, "Environment", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		const char *Found = std::getenv(NeedString(Machine, Args[0], 0).c_str());
		return Value::FromString(Found ? Found : "");
	});
	AddNative(Globals_, *OpsisModule_, "IsInstalled", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		std::string Name = NeedString(Machine, Args[0], 0);
		if (!ValidPackageName(Name)) Machine.Fail(0, "invalid package name", true);
		fs::path Version = fs::path(Machine.Config().DatabaseDirectory) / Name / "version";
		return Value::FromBool(fs::is_regular_file(Version));
	});
	AddNative(Globals_, *OpsisModule_, "FileExists", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		fs::path Full = JoinInside(Machine, NeedString(Machine, Args[0], 0), 0);
		std::error_code Error;
		fs::file_status Status = fs::symlink_status(Full, Error);
		if (Error) return Value::FromBool(false);
		return Value::FromBool(fs::is_regular_file(Status) || fs::is_symlink(Status));
	});
	AddNative(Globals_, *OpsisModule_, "DirectoryExists", 1, 1,
		[](Interpreter &Machine, const std::vector<Value> &Args) {
		fs::path Full = JoinInside(Machine, NeedString(Machine, Args[0], 0), 0);
		std::error_code Error;
		return Value::FromBool(fs::is_directory(Full, Error));
	});
	AddNative(Globals_, *OpsisModule_, "ReadFile", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		fs::path Full = JoinInside(Machine, NeedString(Machine, Args[0], 0), 0);
		std::error_code Error;
		if (!fs::is_regular_file(Full, Error)) Machine.Fail(0, "file does not exist: " + Full.string(), true);
		uintmax_t Size = fs::file_size(Full, Error);
		if (Error || Size > 1048576) Machine.Fail(0, "file is too large to read", true);
		std::ifstream Input(Full, std::ios::binary);
		if (!Input) Machine.Fail(0, "cannot read " + Full.string(), true);
		std::string Text;
		Text.resize(static_cast<size_t>(Size));
		if (Size > 0) Input.read(&Text[0], static_cast<std::streamsize>(Size));
		if (Size > 0 && !Input) Machine.Fail(0, "cannot read " + Full.string(), true);
		return Value::FromString(Text);
	});
	AddNative(Globals_, *OpsisModule_, "WriteFile", 2, 2, [](Interpreter &Machine, const std::vector<Value> &Args) {
		WriteText(Machine, NeedString(Machine, Args[0], 0), NeedString(Machine, Args[1], 0), false, 0);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "AppendFile", 2, 2, [](Interpreter &Machine, const std::vector<Value> &Args) {
		WriteText(Machine, NeedString(Machine, Args[0], 0), NeedString(Machine, Args[1], 0), true, 0);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "MakeDirectory", 1, 2,
		[](Interpreter &Machine, const std::vector<Value> &Args) {
		fs::path Full = JoinInside(Machine, NeedString(Machine, Args[0], 0), 0);
		std::error_code Error;
		fs::create_directories(Full, Error);
		if (Error) Machine.Fail(0, "cannot create directory " + Full.string(), true);
		if (Args.size() == 2) ApplyMode(Full, NeedString(Machine, Args[1], 0));
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "RemoveFile", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		fs::path Full = JoinInside(Machine, NeedString(Machine, Args[0], 0), 0);
		std::error_code Error;
		fs::file_status Status = fs::symlink_status(Full, Error);
		if (Error || (!fs::is_regular_file(Status) && !fs::is_symlink(Status))) {
			Machine.Fail(0, "file does not exist: " + Full.string(), true);
		}
		fs::remove(Full, Error);
		if (Error) Machine.Fail(0, "cannot remove " + Full.string(), true);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "RemoveDirectory", 1, 1,
		[](Interpreter &Machine, const std::vector<Value> &Args) {
		fs::path Root = fs::path(Machine.Config().Sysroot).lexically_normal();
		fs::path Full = JoinInside(Machine, NeedString(Machine, Args[0], 0), 0);
		if (Full == Root) Machine.Fail(0, "refusing to remove the sysroot", true);
		std::error_code Error;
		if (!fs::is_directory(Full, Error)) Machine.Fail(0, "directory does not exist: " + Full.string(), true);
		fs::remove_all(Full, Error);
		if (Error) Machine.Fail(0, "cannot remove " + Full.string(), true);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "MovePath", 2, 2, [](Interpreter &Machine, const std::vector<Value> &Args) {
		fs::path From = JoinInside(Machine, NeedString(Machine, Args[0], 0), 0);
		fs::path To = JoinInside(Machine, NeedString(Machine, Args[1], 0), 0);
		std::error_code Error;
		if (!fs::exists(fs::symlink_status(From, Error))) {
			Machine.Fail(0, "path does not exist: " + From.string(), true);
		}
		fs::create_directories(To.parent_path(), Error);
		if (Error) Machine.Fail(0, "cannot create directory " + To.parent_path().string(), true);
		fs::rename(From, To, Error);
		if (Error) Machine.Fail(0, "cannot move " + From.string(), true);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "Symlink", 2, 2, [](Interpreter &Machine, const std::vector<Value> &Args) {
		std::string Target = NeedString(Machine, Args[0], 0);
		fs::path Link = JoinInside(Machine, NeedString(Machine, Args[1], 0), 0);
		fs::path TargetPath(Target);
		if (Target.empty()) Machine.Fail(0, "empty symlink target", true);
		if (!TargetPath.is_absolute()) {
			fs::path Root = fs::path(Machine.Config().Sysroot).lexically_normal();
			fs::path Resolved = (Link.parent_path() / TargetPath).lexically_normal();
			fs::path Back = Resolved.lexically_relative(Root);
			if (Back.empty() || *Back.begin() == "..") Machine.Fail(0, "symlink target escapes the sysroot", true);
		}
		std::error_code Error;
		fs::create_directories(Link.parent_path(), Error);
		if (Error) Machine.Fail(0, "cannot create directory " + Link.parent_path().string(), true);
		if (fs::exists(fs::symlink_status(Link, Error))) fs::remove(Link, Error);
		fs::create_symlink(Target, Link, Error);
		if (Error) Machine.Fail(0, "cannot create symlink " + Link.string(), true);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "SetMode", 2, 2, [](Interpreter &Machine, const std::vector<Value> &Args) {
		fs::path Full = JoinInside(Machine, NeedString(Machine, Args[0], 0), 0);
		std::error_code Error;
		if (!fs::exists(fs::symlink_status(Full, Error))) {
			Machine.Fail(0, "path does not exist: " + Full.string(), true);
		}
		ApplyMode(Full, NeedString(Machine, Args[1], 0));
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "Run", 1, 16, [](Interpreter &Machine, const std::vector<Value> &Args) {
		std::vector<std::string> Argv;
		for (const Value &Arg : Args) Argv.push_back(NeedString(Machine, Arg, 0));
		if (Argv[0].empty()) Machine.Fail(0, "empty program", true);
		int Status = RunProcess(Argv, false);
		if (Status < 0) Machine.Fail(0, "cannot run " + Argv[0], true);
		return Value::FromInt(Status);
	});
	AddNative(Globals_, *OpsisModule_, "UpdateDesktopDatabase", 0, 0,
		[](Interpreter &Machine, const std::vector<Value> &) {
		RunHostTool(Machine, "update-desktop-database", "usr/share/applications",
			"Updating desktop application database...");
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "UpdateMimeDatabase", 0, 0,
		[](Interpreter &Machine, const std::vector<Value> &) {
		RunHostTool(Machine, "update-mime-database", "usr/share/mime", "Updating MIME database...");
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "UpdateIconCache", 0, 0, [](Interpreter &Machine, const std::vector<Value> &) {
		RunHostTool(Machine, "gtk-update-icon-cache", "usr/share/icons/hicolor", "Updating icon cache...");
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "BeginPackage", 0, 4, [](Interpreter &Machine, const std::vector<Value> &Args) {
		BeginDirectPackage(Machine, Args, 0);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "PackFile", 2, 3, [](Interpreter &Machine, const std::vector<Value> &Args) {
		PackOneFile(Machine, Args, 0);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "PackDirectory", 2, 2, [](Interpreter &Machine, const std::vector<Value> &Args) {
		PackOneDirectory(Machine, Args, 0);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "AddDependency", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		RequireOpen(Machine, 0);
		std::string Name = NeedString(Machine, Args[0], 0);
		if (!PackageToken(Name)) Machine.Fail(0, "invalid dependency name", true);
		PackageSession.Dependencies.push_back(Name);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "FinishPackage", 0, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		FinishDirectPackage(Machine, Args, 0);
		return Value::Null();
	});
	Globals_.Define("Opsis", Value::FromModule(OpsisModule_));
	BindContext();
}

Interpreter::~Interpreter()
{
	ResetDirectPackage();
}

std::vector<StmtPtr> ParseSource(const std::string &Source, const std::string &FileName);

/**
 * CanonicalEntry() - 把入口名收成契约名。
 * @Name: 脚本里的方法名，或宿主请求的入口。
 *
 * Main 与 MAIN 相同。Install 与 INSTALL 相同。Update、Upgrade 与 UPDATE 相同。
 * Remove 与 REMOVE 相同。其它名字原样返回，以便以后增加 VERIFY 这类契约。
 *
 * Return: 契约名。
 */
std::string CanonicalEntry(const std::string &Name)
{
	if (Name == "MAIN" || Name == "Main") return "MAIN";
	if (Name == "INSTALL" || Name == "Install") return "INSTALL";
	if (Name == "UPDATE" || Name == "Update" || Name == "Upgrade") return "UPDATE";
	if (Name == "REMOVE" || Name == "Remove") return "REMOVE";
	return Name;
}

int Interpreter::EntryRankFor(const std::string &MethodName, const std::string &ClassName) const
{
	std::string Canon = CanonicalEntry(MethodName);
	if (!Config_.EntryName.empty()) return Canon == CanonicalEntry(Config_.EntryName) ? 0 : 100;
	if (Canon == "INSTALL") return ClassName.empty() ? 1 : 3;
	if (Canon == "MAIN") return ClassName.empty() ? 2 : 4;
	return 100;
}

bool Interpreter::DerivesFrom(const std::string &Type, const std::string &Base) const
{
	if (Type.empty() || Base.empty()) return false;
	std::string At = Type;
	for (int Hop = 0; Hop < 32; ++Hop) {
		auto Found = BaseOf_.find(At);
		if (Found == BaseOf_.end() || Found->second.empty()) return false;
		if (Found->second == Base) return true;
		At = Found->second;
	}
	return false;
}

void Interpreter::RegisterEntry(const std::shared_ptr<Function> &Fn, int Rank)
{
	if (Rank >= 100) return;
	auto MethodOf = [](const Function &Item) {
		auto Dot = Item.Name.rfind('.');
		return Dot == std::string::npos ? Item.Name : Item.Name.substr(Dot + 1);
	};
	if (!Entry_ || Rank < EntryRank_) {
		Entry_ = Fn;
		EntryRank_ = Rank;
		return;
	}
	if (Rank != EntryRank_) return;
	if (CanonicalEntry(MethodOf(*Fn)) != CanonicalEntry(MethodOf(*Entry_))) return;
	if (Fn->Owner == Entry_->Owner || DerivesFrom(Fn->Owner, Entry_->Owner)) Entry_ = Fn;
}

void Interpreter::DefineFunction(const FunctionStmt *Node, const std::string &ClassName)
{
	auto Fn = std::make_shared<Function>();
	Fn->Owner = ClassName;
	Fn->Name = ClassName.empty() ? Node->Name() : ClassName + "." + Node->Name();
	Fn->ReturnType = Node->ReturnType();
	Fn->Params = Node->Params();
	Fn->Body = Node->Body();
	RegisterEntry(Fn, EntryRankFor(Node->Name(), ClassName));
	if (ClassName.empty()) Globals_.Define(Node->Name(), Value::FromFunction(Fn));
}

void Interpreter::DefineClass(const ClassStmt *Node)
{
	if (Node->Name() == Node->Base()) Fail(Node->Line(), "a class cannot inherit from itself", false);
	std::shared_ptr<Module> BaseMod;
	if (!Node->Base().empty()) {
		try {
			Value BaseValue = Globals_.Get(Node->Base());
			if (!BaseValue.IsModule()) Fail(Node->Line(), "base class is not a class: " + Node->Base(), false);
			BaseMod = BaseValue.ModulePtr();
		} catch (const RuntimeError &) {
			Fail(Node->Line(), "base class is not defined: " + Node->Base(), false);
		}
	}
	BaseOf_[Node->Name()] = Node->Base();
	auto Mod = std::make_shared<Module>();
	if (BaseMod) {
		for (const auto &Member : BaseMod->Members) {
			bool Hidden = false;
			for (const ClassField &Field : Node->Fields()) {
				if (Field.Name == Member.first) Hidden = true;
			}
			for (const auto &Method : Node->Methods()) {
				if (Method->Name() == Member.first) Hidden = true;
			}
			if (Hidden) continue;
			if (Member.second.IsFunction() && Member.second.FunctionPtr()) {
				auto Copy = std::make_shared<Function>(*Member.second.FunctionPtr());
				Copy->Owner = Node->Name();
				Copy->Name = Node->Name() + "." + Member.first;
				Mod->Members[Member.first] = Value::FromFunction(Copy);
				RegisterEntry(Copy, EntryRankFor(Member.first, Node->Name()));
			} else if (!Member.second.IsFunction() && !Member.second.IsNative()) {
				Mod->Members[Member.first] = Member.second;
			}
		}
	}
	{
		Environment Local(&Globals_);
		Local.BindFields(Mod.get());
		EnvironmentGuard Guard(*this, &Local);
		for (const ClassField &Field : Node->Fields()) {
			Value Item = Value::Null();
			if (Field.Init) Item = Field.Init->Evaluate(*this);
			else if (Field.Type == "int") Item = Value::FromInt(0);
			else if (Field.Type == "bool") Item = Value::FromBool(false);
			else if (Field.Type == "string") Item = Value::FromString("");
			else if (IsArrayTypeName(Field.Type)) Item = Value::FromArray(ArrayElementName(Field.Type), {});
			else Fail(Node->Line(), "field requires an initializer: " + Field.Name, false);
			if (!TypeOk(Field.Type, Item)) {
				Fail(Node->Line(), "field initializer type does not match " + Field.Type, false);
			}
			Mod->Members[Field.Name] = Item;
		}
	}
	for (const auto &Method : Node->Methods()) {
		bool BaseHas = BaseMod && BaseMod->Members.count(Method->Name())
			&& BaseMod->Members[Method->Name()].IsFunction();
		if (Method->Override() && !BaseHas) {
			Fail(Method->Body() ? Method->Line() : Node->Line(),
				Method->Name() + " does not override a base method", false);
		}
		if (!Method->Override() && BaseHas) {
			Fail(Method->Body() ? Method->Line() : Node->Line(),
				Method->Name() + " hides " + Node->Base() + "." + Method->Name() + ", use override", false);
		}
		auto Fn = std::make_shared<Function>();
		Fn->Owner = Node->Name();
		Fn->Name = Node->Name() + "." + Method->Name();
		Fn->ReturnType = Method->ReturnType();
		Fn->Params = Method->Params();
		Fn->Body = Method->Body();
		Mod->Members[Method->Name()] = Value::FromFunction(Fn);
		RegisterEntry(Fn, EntryRankFor(Method->Name(), Node->Name()));
	}
	Globals_.Define(Node->Name(), Value::FromModule(Mod));
}

void Interpreter::ImportSource(const std::string &Path, int Line)
{
	fs::path Full = fs::absolute(ResolveSource(*this, Path)).lexically_normal();
	if (!Imported_.insert(Full.string()).second) return;
	if (!fs::is_regular_file(Full)) Fail(Line, "cannot import " + Path, false);
	std::ifstream Input(Full);
	if (!Input) Fail(Line, "cannot import " + Path, false);
	std::stringstream Buffer;
	Buffer << Input.rdbuf();
	OwnedSources_.push_back(ParseSource(Buffer.str(), Full.string()));
	for (const auto &Item : OwnedSources_.back()) {
		if (auto *Imported = dynamic_cast<const ImportStmt *>(Item.get())) Imported->Execute(*this);
		else if (auto *Fn = dynamic_cast<const FunctionStmt *>(Item.get())) DefineFunction(Fn, "");
		else if (auto *Cls = dynamic_cast<const ClassStmt *>(Item.get())) DefineClass(Cls);
		else Item->Execute(*this);
	}
}

void ImportStmt::Execute(Interpreter &Machine) const
{
	Machine.ImportSource(Path_, Line());
}

void Interpreter::BindContext()
{
	auto Context = std::make_shared<Module>();
	auto Put = [&](const char *Name, const std::string &Text) {
		Context->Members[Name] = Value::FromString(Text);
	};
	std::string Entry = CanonicalEntry(Config_.EntryName);
	std::string NewVersion = Config_.NewVersion.empty() ? Config_.PackageVersion : Config_.NewVersion;
	Put("Entry", Entry);
	Put("Sysroot", Config_.Sysroot);
	Put("Architecture", Config_.PackageArchitecture.empty() ? HostArchitecture() : Config_.PackageArchitecture);
	Put("Namespace", Config_.PackageNamespace);
	Put("Name", Config_.PackageName);
	Put("Version", Config_.PackageVersion);
	Put("OldVersion", Config_.OldVersion);
	Put("NewVersion", NewVersion);
	Put("Reason", Config_.Reason.empty() ? Entry : Config_.Reason);
	std::string OldPackage;
	std::string NewPackage;
	if (!Config_.PackageNamespace.empty() && !Config_.PackageName.empty()) {
		if (!Config_.OldVersion.empty()) {
			OldPackage = Config_.PackageNamespace + "." + Config_.PackageName + "@" + Config_.OldVersion;
		}
		if (!NewVersion.empty()) {
			NewPackage = Config_.PackageNamespace + "." + Config_.PackageName + "@" + NewVersion;
		}
	}
	Put("OldPackage", OldPackage);
	Put("NewPackage", NewPackage);
	Context_ = Context;
	Globals_.Define("Context", Value::FromModule(Context));

	auto Runtime = std::make_shared<Module>();
	auto AddDefault = [&](const char *Name) {
		Runtime->Members[Name] = Value::FromNative(Name, 0, 1,
			[](Interpreter &, const std::vector<Value> &) { return Value::FromInt(0); });
	};
	AddDefault("DefaultInstall");
	AddDefault("DefaultUpdate");
	AddDefault("DefaultRemove");
	Globals_.Define("Runtime", Value::FromModule(Runtime));
}

void Interpreter::Run(const std::vector<StmtPtr> &Items)
{
	for (const auto &Item : Items) {
		if (auto *Imported = dynamic_cast<const ImportStmt *>(Item.get())) Imported->Execute(*this);
	}
	std::vector<const Stmt *> Top;
	for (const auto &Item : Items) {
		if (dynamic_cast<const ImportStmt *>(Item.get())) continue;
		if (auto *Fn = dynamic_cast<const FunctionStmt *>(Item.get())) DefineFunction(Fn, "");
		else if (auto *Cls = dynamic_cast<const ClassStmt *>(Item.get())) DefineClass(Cls);
		else Top.push_back(Item.get());
	}
	if (!Top.empty()) {
		for (const Stmt *Item : Top) Item->Execute(*this);
		return;
	}
	if (!Entry_) {
		if (!Config_.EntryName.empty()) return;
		Fail(1, "no MAIN or INSTALL method", false);
	}
	std::vector<Value> Args;
	if (!Entry_->Params.empty()) {
		if (Entry_->Params.size() != 1 || !Context_) Fail(1, Entry_->Name + " entry takes one context", false);
		Args.push_back(Value::FromModule(Context_));
	}
	Value Result = CallFunction(*Entry_, Args, 1);
	if (Result.IsInt() && Result.Int() != 0) {
		Fail(1, "entry returned " + std::to_string(Result.Int()), true);
	}
}

std::vector<StmtPtr> ParseSource(const std::string &Source, const std::string &FileName)
{
	Parser Tokens(Source, FileName);
	return Tokens.Parse();
}

bool ClassHasEntry(const ClassStmt *Node, const std::string &Canon,
	const std::unordered_map<std::string, const ClassStmt *> &ByName, int Depth)
{
	if (!Node || Depth > 16) return false;
	for (const auto &Method : Node->Methods()) {
		if (CanonicalEntry(Method->Name()) == Canon) return true;
	}
	auto Found = ByName.find(Node->Base());
	if (Found == ByName.end()) return false;
	return ClassHasEntry(Found->second, Canon, ByName, Depth + 1);
}

bool DefinesEntry(const std::vector<StmtPtr> &Items, const std::string &Entry)
{
	std::string Canon = CanonicalEntry(Entry);
	std::unordered_map<std::string, const ClassStmt *> ByName;
	for (const auto &Item : Items) {
		if (auto *Cls = dynamic_cast<const ClassStmt *>(Item.get())) ByName[Cls->Name()] = Cls;
	}
	for (const auto &Item : Items) {
		if (auto *Fn = dynamic_cast<const FunctionStmt *>(Item.get())) {
			if (CanonicalEntry(Fn->Name()) == Canon) return true;
		} else if (auto *Cls = dynamic_cast<const ClassStmt *>(Item.get())) {
			if (ClassHasEntry(Cls, Canon, ByName, 0)) return true;
		}
	}
	return false;
}

/**
 * FindEntryProgram() - 在包目录里找出定义了该入口的程序文件。
 * @Root: 包根目录。
 * @Entry: 入口契约。
 *
 * package.opsis 和 lifecycle.opsis 只有在定义了该入口时才选中。
 * 约定文件即使只有顶层语句也会选中，这样旧的脚本列表仍然执行。
 *
 * Return: 选中的文件。没有时返回空路径。
 */
fs::path FindEntryProgram(const fs::path &Root, const std::string &Entry)
{
	std::string Canon = CanonicalEntry(Entry);
	std::vector<std::pair<std::string, bool>> Names = {
		{"package.opsis", false},
		{"lifecycle.opsis", false}
	};
	auto AddScene = [&](const std::string &Stem) {
		Names.emplace_back(Stem + ".opsis", true);
	};
	if (Canon == "INSTALL") AddScene("install");
	else if (Canon == "UPDATE") {
		AddScene("update");
		AddScene("upgrade");
	} else if (Canon == "REMOVE") AddScene("remove");
	else if (Canon == "MAIN") AddScene("main");
	else {
		std::string Lower = Canon;
		for (char &Ch : Lower) Ch = static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
		AddScene(Lower);
	}
	for (const auto &Name : Names) {
		for (const fs::path &Dir : {Root / "scripts", Root}) {
			fs::path Path = Dir / Name.first;
			if (!fs::is_regular_file(Path)) continue;
			try {
				std::ifstream Input(Path);
				if (!Input) continue;
				std::stringstream Buffer;
				Buffer << Input.rdbuf();
				auto Items = ParseSource(Buffer.str(), Path.string());
				if (DefinesEntry(Items, Entry) || (Name.second && !Items.empty())) return Path;
			} catch (const RuntimeError &) {
				return Path;
			}
		}
	}
	return {};
}

} // namespace

const char *HostArchitecture()
{
#if defined(__aarch64__)
	return "aarch64";
#elif defined(__riscv) && defined(__riscv_xlen) && (__riscv_xlen == 64)
	return "riscv64";
#elif defined(__x86_64__)
	return "x86_64";
#else
	static std::string Name;
	if (Name.empty()) {
		utsname Info{};
		if (uname(&Info) == 0 && Info.machine[0] != '\0') Name = Info.machine;
		else Name = "unknown";
	}
	return Name.c_str();
#endif
}

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
	auto Take = [](const char *Name) {
		const char *Value = std::getenv(Name);
		return Value && *Value ? std::string(Value) : std::string();
	};
	Config.PackageNamespace = Take("OPSIS_PKG_NAMESPACE");
	Config.PackageName = Take("OPSIS_PKG_NAME");
	Config.PackageVersion = Take("OPSIS_PKG_VERSION");
	Config.PackageDescription = Take("OPSIS_PKG_DESCRIPTION");
	Config.PackageArchitecture = Take("OPSIS_PKG_ARCH");
	Config.PackageOutput = Take("OPSIS_PKG_OUTPUT");
	Config.BuildDirectory = Take("OPSIS_BUILD_DIR");
	return Config;
}

void ApplyScene(RuntimeConfig &Config, const PackageScene &Scene)
{
	if (!Scene.Namespace.empty()) Config.PackageNamespace = Scene.Namespace;
	if (!Scene.Name.empty()) Config.PackageName = Scene.Name;
	if (!Scene.Version.empty()) Config.PackageVersion = Scene.Version;
	if (!Scene.OldVersion.empty()) Config.OldVersion = Scene.OldVersion;
	if (!Scene.NewVersion.empty()) Config.NewVersion = Scene.NewVersion;
	if (!Scene.Architecture.empty()) Config.PackageArchitecture = Scene.Architecture;
	if (!Scene.Reason.empty()) Config.Reason = Scene.Reason;
}

RuntimeConfig SceneConfig(const std::string &Entry, const std::string &Sysroot, bool AllowNonRoot,
	const PackageScene &Scene)
{
	RuntimeConfig Config = LoadConfig();
	Config.Sysroot = Sysroot;
	Config.AllowNonRoot = AllowNonRoot || Config.AllowNonRoot;
	if (geteuid() != 0 && Sysroot != "/") Config.AllowNonRoot = true;
	const char *Database = std::getenv("OPSIS_DB_DIR");
	if (!(Database && *Database)) {
		Config.DatabaseDirectory = (fs::path(Sysroot) / "var/lib/okrapm/db").string();
	}
	Config.EntryName = CanonicalEntry(Entry);
	ApplyScene(Config, Scene);
	if (Config.Reason.empty()) Config.Reason = Config.EntryName;
	return Config;
}

int RunSource(const std::string &Source, const std::string &FileName, const RuntimeConfig &Config)
{
	try {
		RuntimeConfig Local = Config;
		if (Local.PackOnly && Local.EntryName.empty()) Local.EntryName = "MAIN";
		if (!Local.PackOnly && !fs::is_directory(Local.Sysroot)) {
			std::cerr << "\033[1;31m[OPSIS:FAIL]\033[0m Target sysroot does not exist: " << Local.Sysroot << "\n";
			return 1;
		}
		std::error_code Error;
		if (!Local.PackOnly) fs::create_directories(Local.DatabaseDirectory, Error);
		if (Error) {
			std::cerr << "\033[1;31m[OPSIS:FAIL]\033[0m Cannot create database directory: "
				<< Local.DatabaseDirectory << "\n";
			return 1;
		}
		auto Items = ParseSource(Source, FileName);
		Interpreter Machine(Local, FileName);
		Machine.Run(Items);
		return 0;
	} catch (const ReturnSignal &) {
		return 0;
	} catch (const ThrowSignal &Signal) {
		std::cerr << "\033[1;31m[OPSIS:FAIL]\033[0m " << Signal.TypeName;
		if (!Signal.Message.empty()) std::cerr << ": " << Signal.Message;
		std::cerr << "\n";
		return 1;
	} catch (const BreakSignal &) {
		std::cerr << FileName << ": break is outside a loop\n";
		return 2;
	} catch (const ContinueSignal &) {
		std::cerr << FileName << ": continue is outside a loop\n";
		return 2;
	} catch (const GotoCaseSignal &) {
		std::cerr << FileName << ": goto case is outside a switch\n";
		return 2;
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

int RunScriptFile(const std::string &ScriptPath, const std::string &Entry, const std::string &Sysroot,
	bool AllowNonRoot, const PackageScene &Scene)
{
	if (!fs::is_regular_file(ScriptPath)) return 0;
	RuntimeConfig Config = SceneConfig(Entry, Sysroot, AllowNonRoot, Scene);
	Config.BaseDirectory = fs::absolute(ScriptPath).parent_path().string();
	return RunFile(ScriptPath, Config);
}

int RunLifecycleScript(const std::string &PackageDir, const std::string &Entry, const std::string &Sysroot,
	bool AllowNonRoot, const PackageScene &Scene)
{
	fs::path Script = FindEntryProgram(PackageDir, Entry);
	if (Script.empty()) return 0;
	RuntimeConfig Config = SceneConfig(Entry, Sysroot, AllowNonRoot, Scene);
	Config.BaseDirectory = fs::absolute(PackageDir).string();
	return RunFile(Script.string(), Config);
}

int RunPackageScript(const std::string &PackageDir, const std::string &Sysroot, bool AllowNonRoot)
{
	return RunLifecycleScript(PackageDir, "INSTALL", Sysroot, AllowNonRoot, {});
}

} // namespace Opsis
