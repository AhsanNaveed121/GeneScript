#ifndef GENESCRIPT_AST_H
#define GENESCRIPT_AST_H

#include <string>
#include <vector>
#include <memory>
#include <iostream>

enum DataType {
    TYPE_UNKNOWN = 0,
    TYPE_DNA,
    TYPE_RNA,
    TYPE_INT,
    TYPE_BOOL,
    TYPE_VOID
};

std::string dataTypeToString(DataType t);

struct SourceLoc {
    int line;
    int col;
};

// Runtime value for interpreter
struct Value {
    DataType type;
    std::string str_val; // For DNA/RNA sequences or single base
    long long int_val;   // For integer loop counters, bounds, indices, #s
    bool bool_val;       // For bound / unbound

    Value() : type(TYPE_UNKNOWN), str_val(""), int_val(0), bool_val(false) {}
    static Value makeDNA(const std::string& s) {
        Value v;
        v.type = TYPE_DNA;
        v.str_val = s;
        return v;
    }
    static Value makeRNA(const std::string& s) {
        Value v;
        v.type = TYPE_RNA;
        v.str_val = s;
        return v;
    }
    static Value makeInt(long long n) {
        Value v;
        v.type = TYPE_INT;
        v.int_val = n;
        return v;
    }
    static Value makeBool(bool b) {
        Value v;
        v.type = TYPE_BOOL;
        v.bool_val = b;
        return v;
    }
};

class ASTVisitor;

// Base AST Node
class ASTNode {
public:
    SourceLoc loc;
    virtual ~ASTNode() = default;
};

// Base Expression
class ExprAST : public ASTNode {
public:
    DataType deduced_type = TYPE_UNKNOWN;
    virtual ~ExprAST() = default;
    virtual Value evaluate(class InterpreterContext& ctx) = 0;
};

// Literals: "ATGC", 'G', ""
class LiteralExprAST : public ExprAST {
public:
    std::string value;
    bool is_single_base;
    LiteralExprAST(SourceLoc loc, const std::string& val, bool single_base = false)
        : value(val), is_single_base(single_base) { this->loc = loc; }
    Value evaluate(class InterpreterContext& ctx) override;
};

// Integer Literal (for cycle bounds: cycle i from 0 to #s)
class IntLiteralExprAST : public ExprAST {
public:
    long long value;
    IntLiteralExprAST(SourceLoc loc, long long val) : value(val) {
        this->loc = loc;
        this->deduced_type = TYPE_INT;
    }
    Value evaluate(class InterpreterContext& ctx) override;
};

// Boolean Literal: bound (true), unbound (false)
class BoolLiteralExprAST : public ExprAST {
public:
    bool value;
    BoolLiteralExprAST(SourceLoc loc, bool val) : value(val) {
        this->loc = loc;
        this->deduced_type = TYPE_BOOL;
    }
    Value evaluate(class InterpreterContext& ctx) override;
};

// Variable access: g, seq, i
class VarExprAST : public ExprAST {
public:
    std::string name;
    VarExprAST(SourceLoc loc, const std::string& name) : name(name) { this->loc = loc; }
    Value evaluate(class InterpreterContext& ctx) override;
};

// Binary operations: fuse a b, comparison (=, <, >, <=, >=), logical (both, either)
enum BinaryOp {
    OP_FUSE,
    OP_EQ,
    OP_LT,
    OP_GT,
    OP_LE,
    OP_GE,
    OP_BOTH,
    OP_EITHER
};

class BinaryExprAST : public ExprAST {
public:
    BinaryOp op;
    std::unique_ptr<ExprAST> lhs;
    std::unique_ptr<ExprAST> rhs;
    BinaryExprAST(SourceLoc loc, BinaryOp op, std::unique_ptr<ExprAST> lhs, std::unique_ptr<ExprAST> rhs)
        : op(op), lhs(std::move(lhs)), rhs(std::move(rhs)) { this->loc = loc; }
    Value evaluate(class InterpreterContext& ctx) override;
};

// Unary operations: ~d (reverse complement), lacks cond (logical not), #s (length)
enum UnaryOp {
    OP_REVCOMP, // ~
    OP_LACKS,   // lacks
    OP_LEN      // #
};

class UnaryExprAST : public ExprAST {
public:
    UnaryOp op;
    std::unique_ptr<ExprAST> operand;
    UnaryExprAST(SourceLoc loc, UnaryOp op, std::unique_ptr<ExprAST> operand)
        : op(op), operand(std::move(operand)) { this->loc = loc; }
    Value evaluate(class InterpreterContext& ctx) override;
};

// Built-in operations: head(s), tail(s), transcribe(d)
enum BuiltinOp {
    BUILTIN_HEAD,
    BUILTIN_TAIL,
    BUILTIN_TRANSCRIBE
};

class BuiltinCallExprAST : public ExprAST {
public:
    BuiltinOp op;
    std::unique_ptr<ExprAST> arg;
    BuiltinCallExprAST(SourceLoc loc, BuiltinOp op, std::unique_ptr<ExprAST> arg)
        : op(op), arg(std::move(arg)) { this->loc = loc; }
    Value evaluate(class InterpreterContext& ctx) override;
};

// Indexing: s@i
class IndexExprAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> seq;
    std::unique_ptr<ExprAST> index;
    IndexExprAST(SourceLoc loc, std::unique_ptr<ExprAST> seq, std::unique_ptr<ExprAST> index)
        : seq(std::move(seq)), index(std::move(index)) { this->loc = loc; }
    Value evaluate(class InterpreterContext& ctx) override;
};

// Function call: merge(a, b)
class CallExprAST : public ExprAST {
public:
    std::string callee;
    std::vector<std::unique_ptr<ExprAST>> args;
    CallExprAST(SourceLoc loc, const std::string& callee, std::vector<std::unique_ptr<ExprAST>> args)
        : callee(callee), args(std::move(args)) { this->loc = loc; }
    Value evaluate(class InterpreterContext& ctx) override;
};

// Base Statement
enum ExecStatus {
    STATUS_NORMAL,
    STATUS_RETURN,
    STATUS_HALT, // break
    STATUS_SKIP  // continue
};

class StmtAST : public ASTNode {
public:
    virtual ~StmtAST() = default;
    virtual ExecStatus execute(class InterpreterContext& ctx, Value& return_val) = 0;
};

// Value-first Declaration: "ATGC" -> dna g
class DeclStmtAST : public StmtAST {
public:
    std::unique_ptr<ExprAST> init_expr;
    DataType declared_type;
    std::string var_name;
    DeclStmtAST(SourceLoc loc, std::unique_ptr<ExprAST> init, DataType t, const std::string& name)
        : init_expr(std::move(init)), declared_type(t), var_name(name) { this->loc = loc; }
    ExecStatus execute(class InterpreterContext& ctx, Value& return_val) override;
};

// Value-first Assignment: tail(g) -> g
class AssignStmtAST : public StmtAST {
public:
    std::unique_ptr<ExprAST> expr;
    std::string var_name;
    AssignStmtAST(SourceLoc loc, std::unique_ptr<ExprAST> expr, const std::string& name)
        : expr(std::move(expr)), var_name(name) { this->loc = loc; }
    ExecStatus execute(class InterpreterContext& ctx, Value& return_val) override;
};

// Report (print): report <expr>
class ReportStmtAST : public StmtAST {
public:
    std::unique_ptr<ExprAST> expr;
    ReportStmtAST(SourceLoc loc, std::unique_ptr<ExprAST> expr)
        : expr(std::move(expr)) { this->loc = loc; }
    ExecStatus execute(class InterpreterContext& ctx, Value& return_val) override;
};

// Express (return): express <expr>
class ExpressStmtAST : public StmtAST {
public:
    std::unique_ptr<ExprAST> expr;
    ExpressStmtAST(SourceLoc loc, std::unique_ptr<ExprAST> expr)
        : expr(std::move(expr)) { this->loc = loc; }
    ExecStatus execute(class InterpreterContext& ctx, Value& return_val) override;
};

// When-Otherwise Conditional:
// when cond ... otherwise when cond ... otherwise ... stop
struct CondBranch {
    std::unique_ptr<ExprAST> condition;
    std::vector<std::unique_ptr<StmtAST>> body;
};

class WhenStmtAST : public StmtAST {
public:
    std::vector<CondBranch> branches;
    std::vector<std::unique_ptr<StmtAST>> otherwise_body;
    WhenStmtAST(SourceLoc loc) { this->loc = loc; }
    ExecStatus execute(class InterpreterContext& ctx, Value& return_val) override;
};

// Counted loop: cycle i from a to b ... stop
class CycleFromToStmtAST : public StmtAST {
public:
    std::string counter_var;
    std::unique_ptr<ExprAST> from_expr;
    std::unique_ptr<ExprAST> to_expr;
    std::vector<std::unique_ptr<StmtAST>> body;
    CycleFromToStmtAST(SourceLoc loc, const std::string& counter,
                       std::unique_ptr<ExprAST> from, std::unique_ptr<ExprAST> to,
                       std::vector<std::unique_ptr<StmtAST>> body)
        : counter_var(counter), from_expr(std::move(from)), to_expr(std::move(to)), body(std::move(body)) {
        this->loc = loc;
    }
    ExecStatus execute(class InterpreterContext& ctx, Value& return_val) override;
};

// Loop across sequence: cycle i across s ... stop
// Visits every base in s; i is the integer index (0 to len-1)
class CycleAcrossStmtAST : public StmtAST {
public:
    std::string counter_var;
    std::unique_ptr<ExprAST> seq_expr;
    std::vector<std::unique_ptr<StmtAST>> body;
    CycleAcrossStmtAST(SourceLoc loc, const std::string& counter,
                      std::unique_ptr<ExprAST> seq,
                      std::vector<std::unique_ptr<StmtAST>> body)
        : counter_var(counter), seq_expr(std::move(seq)), body(std::move(body)) {
        this->loc = loc;
    }
    ExecStatus execute(class InterpreterContext& ctx, Value& return_val) override;
};

// Loop while condition is false until true: grow until <cond> ... stop
class GrowUntilStmtAST : public StmtAST {
public:
    std::unique_ptr<ExprAST> cond_expr;
    std::vector<std::unique_ptr<StmtAST>> body;
    GrowUntilStmtAST(SourceLoc loc, std::unique_ptr<ExprAST> cond,
                     std::vector<std::unique_ptr<StmtAST>> body)
        : cond_expr(std::move(cond)), body(std::move(body)) {
        this->loc = loc;
    }
    ExecStatus execute(class InterpreterContext& ctx, Value& return_val) override;
};

// Halt (break)
class HaltStmtAST : public StmtAST {
public:
    HaltStmtAST(SourceLoc loc) { this->loc = loc; }
    ExecStatus execute(class InterpreterContext& ctx, Value& return_val) override;
};

// Skip (continue)
class SkipStmtAST : public StmtAST {
public:
    SkipStmtAST(SourceLoc loc) { this->loc = loc; }
    ExecStatus execute(class InterpreterContext& ctx, Value& return_val) override;
};

// Function parameter
struct ParamDecl {
    DataType type;
    std::string name;
    SourceLoc loc;
};

// Function declaration: gene <name>(<params>) ... stop
class GeneDeclAST : public ASTNode {
public:
    std::string name;
    std::vector<ParamDecl> params;
    std::vector<std::unique_ptr<StmtAST>> body;
    GeneDeclAST(SourceLoc loc, const std::string& name, std::vector<ParamDecl> params,
                std::vector<std::unique_ptr<StmtAST>> body)
        : name(name), params(std::move(params)), body(std::move(body)) {
        this->loc = loc;
    }
};

// Entire Program: list of genes (and optional top-level stmts for testing error.gene)
class ProgramAST : public ASTNode {
public:
    std::vector<std::unique_ptr<GeneDeclAST>> genes;
    std::vector<std::unique_ptr<StmtAST>> top_level_stmts;
};

#endif // GENESCRIPT_AST_H
