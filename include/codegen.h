#ifndef GENESCRIPT_CODEGEN_H
#define GENESCRIPT_CODEGEN_H

// Phase 2 LLVM backend: walks the (semantically checked) AST and emits
// textual LLVM IR (.ll).  The LLVM C++ API is deliberately not used.
//
// Layering:
//   IRContext     - generic, GeneScript-agnostic IR text builder: module-level
//                   globals/declarations, per-function buffers, temporaries,
//                   labels, block-termination tracking, lexical scopes.
//   CodeGenerator - GeneScript-specific lowering of AST nodes onto IRContext,
//                   following the runtime ABI in docs/LLVM_BACKEND.md.
//
// Supported so far (Parts 1-2): gene definitions, `gene nucleus()` as the
// entry point, sequence/base literals, declarations, assignments, variable
// reads, report, head/tail/transcribe, fuse, ~, s@<literal>, #s (lowering
// only), = and the ordering operators.  Every other construct produces a
// clean "not supported yet" diagnostic instead of wrong code.

#include "ast.h"
#include "semantics.h"

#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// LLVM type used for every GeneScript DNA/RNA value (pointer to GsSeq).
// ABI: must agree with runtime/gs_runtime.h.
constexpr const char* GS_SEQ_IR_TYPE = "ptr";

// Result of lowering an expression.
struct IRValue {
    std::string ref;               // operand text: "%t3", "@.seq.0", "1", ...
    DataType type = TYPE_UNKNOWN;  // GeneScript type
    bool owned = false;            // true: this is a +1 reference the consumer
                                   // must release (see ownership rules).
};

// A local variable visible in the current lexical scope.
struct IRVariable {
    std::string name;   // GeneScript name
    std::string slot;   // alloca'd pointer holding the value, e.g. "%v.g.2"
    DataType type;
};

class IRContext {
public:
    // ---- Module level ----------------------------------------------------
    // Interns a sequence literal as an immortal constant GsSeq global and
    // returns its symbol ("@.seq.N").  Identical literals share one global.
    std::string internSequence(int kind, const std::string& bases);

    // Records `declare <signature>` for an external (runtime) function; each
    // distinct declaration is printed once.
    void declareExternal(const std::string& signature);

    // ---- Functions -------------------------------------------------------
    // Starts `define <ret> @<name>(<params>)`.  `params` are full LLVM
    // parameter texts such as "ptr %p.s".  Resets temporary/label numbering.
    void beginFunction(const std::string& ret_type, const std::string& name,
                       const std::vector<std::string>& params);
    // Closes the current function.  If the last block is still open it is
    // terminated with `default_terminator` (e.g. "ret void").
    void endFunction(const std::string& default_terminator);

    // Fresh SSA temporary / label names (unique within the function).
    std::string newTemp();
    std::string newLabel(const std::string& hint);

    // Emits one instruction into the current block.  Emitting after a
    // terminator opens an unreachable continuation block automatically so
    // the output is always well-formed.
    void emit(const std::string& instr);
    // Emits a terminator (ret/br/unreachable) and closes the current block.
    void emitTerminator(const std::string& instr);
    // Starts a new basic block.  The previous block must be terminated
    // (if not, a fall-through `br` is inserted).
    void startBlock(const std::string& label);
    bool blockTerminated() const { return terminated_; }

    // Emits an `alloca` in the function's entry block; returns the pointer.
    // A non-empty `init` is stored into the slot in the entry block too
    // (sequence slots start as `null`, which gs_seq_release ignores).
    std::string emitAlloca(const std::string& type, const std::string& hint,
                           const std::string& init = "");

    // ---- Scopes ----------------------------------------------------------
    void pushScope();
    // Pops the innermost scope and returns its variables (declaration order)
    // so the caller can emit cleanup (e.g. release sequences).
    std::vector<IRVariable> popScope();
    // Declares a variable in the innermost scope; returns false if already
    // declared in that same scope.
    bool declareVar(const IRVariable& var);
    // Innermost-first lookup; nullptr if not found.
    const IRVariable* lookupVar(const std::string& name) const;
    // All live variables, innermost scope first (for early-exit cleanup).
    std::vector<IRVariable> liveVars() const;

    // Final module text.
    std::string str(const std::string& source_name) const;

    static std::string escapeBytes(const std::string& s);  // for c"..."
    static std::string hostTargetTriple();                 // "" if unknown host

private:
    std::ostringstream globals_;
    std::vector<std::string> declarations_;
    std::set<std::string> declared_;
    std::map<std::pair<int, std::string>, std::string> seq_literals_;
    std::ostringstream functions_;

    // Current-function state.
    bool in_function_ = false;
    std::string fn_header_;
    std::ostringstream fn_allocas_;
    std::ostringstream fn_body_;
    bool terminated_ = false;
    int temp_counter_ = 0;
    int label_counter_ = 0;

    std::vector<std::vector<IRVariable>> scopes_;
};

class CodeGenerator {
public:
    // `sem` must be the analyzer that successfully checked `prog`; its gene
    // signatures (inferred return types) are used for function signatures.
    CodeGenerator(const std::string& source_name, const SemanticAnalyzer& sem);

    // Lowers the program.  On success writes the module to `out` and returns
    // true; otherwise fills `errors` (one formatted line each) and returns false.
    bool generate(ProgramAST& prog, std::ostream& out);

    std::vector<std::string> errors;

    // Mangled LLVM symbol for a gene ("nucleus" -> "gs_gene_nucleus").
    static std::string geneSymbol(const std::string& gene_name);

private:
    std::string source_name_;
    const SemanticAnalyzer& sem_;
    IRContext ctx_;
    DataType current_ret_ = TYPE_DNA;  // sequence type of the gene being lowered

    void genGene(GeneDeclAST& gene);
    void genEntryPoint();
    void genStmt(StmtAST* stmt);
    IRValue genExpr(ExprAST* expr);

    void genDecl(DeclStmtAST* decl);
    void genAssign(AssignStmtAST* assign);
    void genReport(ReportStmtAST* rep);
    IRValue genVar(VarExprAST* var);
    IRValue genBinary(BinaryExprAST* bin);
    // Calls a runtime `ptr fn(ptr)` on a sequence operand, releasing the
    // operand if owned; the result is owned and has type `result_type`
    // (TYPE_UNKNOWN = same type as the operand).
    IRValue genSeqUnary(ExprAST* operand, const char* fn, DataType result_type);

    // Ownership helpers (see docs/LLVM_BACKEND.md, "Ownership").
    void releaseIfOwned(const IRValue& v);
    void releaseScopeVars(const std::vector<IRVariable>& vars);
    // Returns an owned reference to v's sequence (retains if borrowed).
    std::string makeOwned(const IRValue& v);

    static std::string llvmType(DataType t);
    void unsupported(const ASTNode* node, const std::string& what);
};

#endif // GENESCRIPT_CODEGEN_H
