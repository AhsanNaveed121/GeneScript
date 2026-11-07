// Phase 2 LLVM backend: AST -> textual LLVM IR.  See include/codegen.h for the
// layering and docs/LLVM_BACKEND.md for the runtime ABI.

#include "codegen.h"
#include "../runtime/gs_runtime.h"  // GS_KIND_* / GS_REFCOUNT_IMMORTAL (ABI)

#include <cstdio>

// ===========================================================================
// IRContext
// ===========================================================================

std::string IRContext::escapeBytes(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') {
            out.push_back(static_cast<char>(c));
        } else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "\\%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string IRContext::internSequence(int kind, const std::string& bases) {
    auto key = std::make_pair(kind, bases);
    auto it = seq_literals_.find(key);
    if (it != seq_literals_.end()) return it->second;

    std::string sym = "@.seq." + std::to_string(seq_literals_.size());
    size_t n = bases.size() + 1;  // + NUL terminator
    // ABI: { refcount, len, kind, data } == struct GsSeq (gs_runtime.h).
    globals_ << sym << " = private unnamed_addr constant { i64, i64, i32, [" << n
             << " x i8] } { i64 " << GS_REFCOUNT_IMMORTAL << ", i64 " << bases.size()
             << ", i32 " << kind << ", [" << n << " x i8] c\"" << escapeBytes(bases)
             << "\\00\" }, align 8\n";
    seq_literals_[key] = sym;
    return sym;
}

void IRContext::declareExternal(const std::string& signature) {
    if (declared_.insert(signature).second) {
        declarations_.push_back("declare " + signature);
    }
}

void IRContext::beginFunction(const std::string& ret_type, const std::string& name,
                              const std::vector<std::string>& params) {
    in_function_ = true;
    terminated_ = false;
    current_block_ = "entry";
    temp_counter_ = 0;
    label_counter_ = 0;
    fn_allocas_.str("");
    fn_body_.str("");
    std::string header = "define " + ret_type + " @" + name + "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) header += ", ";
        header += params[i];
    }
    fn_header_ = header + ") {";
}

void IRContext::endFunction(const std::string& default_terminator) {
    if (!terminated_) emitTerminator(default_terminator);
    functions_ << fn_header_ << "\nentry:\n" << fn_allocas_.str() << fn_body_.str() << "}\n\n";
    in_function_ = false;
}

std::string IRContext::newTemp() { return "%t" + std::to_string(temp_counter_++); }

std::string IRContext::newLabel(const std::string& hint) {
    return hint + "." + std::to_string(label_counter_++);
}

void IRContext::emit(const std::string& instr) {
    if (terminated_) startBlock(newLabel("dead"));  // code after ret/br
    fn_body_ << "  " << instr << "\n";
}

void IRContext::emitTerminator(const std::string& instr) {
    emit(instr);
    terminated_ = true;
}

void IRContext::startBlock(const std::string& label) {
    if (!terminated_) fn_body_ << "  br label %" << label << "\n";
    fn_body_ << label << ":\n";
    current_block_ = label;
    terminated_ = false;
}

std::string IRContext::emitAlloca(const std::string& type, const std::string& hint,
                                  const std::string& init) {
    std::string slot = "%v." + hint + "." + std::to_string(temp_counter_++);
    fn_allocas_ << "  " << slot << " = alloca " << type << ", align 8\n";
    if (!init.empty()) {
        fn_allocas_ << "  store " << type << " " << init << ", ptr " << slot << ", align 8\n";
    }
    return slot;
}

void IRContext::pushScope() { scopes_.emplace_back(); }

std::vector<IRVariable> IRContext::popScope() {
    std::vector<IRVariable> vars = std::move(scopes_.back());
    scopes_.pop_back();
    return vars;
}

bool IRContext::declareVar(const IRVariable& var) {
    for (const auto& v : scopes_.back()) {
        if (v.name == var.name) return false;
    }
    scopes_.back().push_back(var);
    return true;
}

const IRVariable* IRContext::lookupVar(const std::string& name) const {
    for (auto s = scopes_.rbegin(); s != scopes_.rend(); ++s) {
        for (auto v = s->rbegin(); v != s->rend(); ++v) {
            if (v->name == name) return &*v;
        }
    }
    return nullptr;
}

std::vector<IRVariable> IRContext::liveVars() const {
    std::vector<IRVariable> out;
    for (auto s = scopes_.rbegin(); s != scopes_.rend(); ++s) {
        out.insert(out.end(), s->rbegin(), s->rend());
    }
    return out;
}

std::vector<IRVariable> IRContext::varsFrom(size_t depth) const {
    std::vector<IRVariable> out;
    for (size_t s = scopes_.size(); s > depth; --s) {
        const auto& scope = scopes_[s - 1];
        out.insert(out.end(), scope.rbegin(), scope.rend());
    }
    return out;
}

std::string IRContext::hostTargetTriple() {
    // Triple of the machine the compiler itself was built for (AOT to the
    // same Linux host).  Empty -> no `target triple` line; clang then uses its
    // default and emits -Woverride-module.
#if defined(__linux__) && defined(__x86_64__)
    return "x86_64-pc-linux-gnu";
#elif defined(__linux__) && defined(__aarch64__)
    return "aarch64-unknown-linux-gnu";
#else
    return "";
#endif
}

std::string IRContext::str(const std::string& source_name) const {
    std::ostringstream m;
    m << "; GeneScript LLVM IR -- generated from " << source_name << "\n"
      << "; Link with: clang <this>.ll runtime/gs_runtime.c -o <program>\n"
      << "source_filename = \"" << escapeBytes(source_name) << "\"\n";
    std::string triple = hostTargetTriple();
    if (!triple.empty()) m << "target triple = \"" << triple << "\"\n";
    m << "\n";
    if (!globals_.str().empty()) {
        m << "; Sequence literals: immortal GsSeq objects { refcount, len, kind, data }\n"
          << globals_.str() << "\n";
    }
    if (!declarations_.empty()) {
        m << "; GeneScript runtime (runtime/gs_runtime.c)\n";
        for (const auto& d : declarations_) m << d << "\n";
        m << "\n";
    }
    m << functions_.str();
    return m.str();
}

// ===========================================================================
// CodeGenerator
// ===========================================================================

namespace {
// ABI: runtime entry points used by generated code (see gs_runtime.h).
const char* RT_REPORT  = "void @gs_report(ptr)";
const char* RT_RETAIN  = "ptr @gs_seq_retain(ptr)";
const char* RT_RELEASE = "void @gs_seq_release(ptr)";
const char* RT_LEN        = "i64 @gs_seq_len(ptr)";
const char* RT_AT         = "ptr @gs_seq_at(ptr, i64)";
const char* RT_FUSE       = "ptr @gs_seq_fuse(ptr, ptr)";
const char* RT_EQUAL      = "i32 @gs_seq_equal(ptr, ptr)";
const char* RT_CMP_BASES  = "i32 @gs_seq_compare_bases(ptr, ptr, i64)";
// `ptr fn(ptr)` operations: symbol names; the declaration is built from them.
const char* RT_HEAD       = "gs_seq_head";
const char* RT_TAIL       = "gs_seq_tail";
const char* RT_REVCOMP    = "gs_seq_revcomp";
const char* RT_TRANSCRIBE = "gs_seq_transcribe";

int seqKind(DataType t) { return t == TYPE_RNA ? GS_KIND_RNA : GS_KIND_DNA; }

bool isSequence(DataType t) { return t == TYPE_DNA || t == TYPE_RNA; }

std::string stmtName(StmtAST* s) {
    if (dynamic_cast<DeclStmtAST*>(s)) return "variable declaration ('-> dna/rna name')";
    if (dynamic_cast<AssignStmtAST*>(s)) return "assignment ('-> name')";
    if (dynamic_cast<ExpressStmtAST*>(s)) return "'express'";
    if (dynamic_cast<WhenStmtAST*>(s)) return "'when ... otherwise ... stop'";
    if (dynamic_cast<CycleFromToStmtAST*>(s)) return "'cycle ... from ... to'";
    if (dynamic_cast<CycleAcrossStmtAST*>(s)) return "'cycle ... across'";
    if (dynamic_cast<GrowUntilStmtAST*>(s)) return "'grow until'";
    if (dynamic_cast<HaltStmtAST*>(s)) return "'halt'";
    if (dynamic_cast<SkipStmtAST*>(s)) return "'skip'";
    return "this statement";
}

std::string exprName(ExprAST* e) {
    if (dynamic_cast<IntLiteralExprAST*>(e)) return "integer literal";
    if (dynamic_cast<BoolLiteralExprAST*>(e)) return "'bound'/'unbound'";
    if (dynamic_cast<VarExprAST*>(e)) return "variable reference";
    if (dynamic_cast<CallExprAST*>(e)) return "gene call";
    if (dynamic_cast<IndexExprAST*>(e)) return "indexing ('s@i')";
    if (auto* b = dynamic_cast<BinaryExprAST*>(e)) {
        switch (b->op) {
            case OP_FUSE: return "'fuse'";
            case OP_BOTH: return "'both'";
            case OP_EITHER: return "'either'";
            default: return "comparison";
        }
    }
    if (auto* u = dynamic_cast<UnaryExprAST*>(e)) {
        switch (u->op) {
            case OP_REVCOMP: return "reverse complement ('~')";
            case OP_LACKS: return "'lacks'";
            case OP_LEN: return "length ('#')";
        }
    }
    if (auto* c = dynamic_cast<BuiltinCallExprAST*>(e)) {
        switch (c->op) {
            case BUILTIN_HEAD: return "'head()'";
            case BUILTIN_TAIL: return "'tail()'";
            case BUILTIN_TRANSCRIBE: return "'transcribe()'";
        }
    }
    return "this expression";
}
}  // namespace

CodeGenerator::CodeGenerator(const std::string& source_name, const SemanticAnalyzer& sem)
    : source_name_(source_name), sem_(sem) {}

std::string CodeGenerator::geneSymbol(const std::string& gene_name) {
    return "gs_gene_" + gene_name;
}

std::string CodeGenerator::llvmType(DataType t) {
    switch (t) {
        case TYPE_DNA:
        case TYPE_RNA: return GS_SEQ_IR_TYPE;
        case TYPE_BOOL: return "i1";
        case TYPE_INT: return "i64";
        default: return "void";
    }
}

void CodeGenerator::unsupported(const ASTNode* node, const std::string& what) {
    errors.push_back(source_name_ + ":" + std::to_string(node->loc.line) + ": " + what +
                     " is not supported by the LLVM backend yet.\n");
}

bool CodeGenerator::generate(ProgramAST& prog, std::ostream& out) {
    for (auto& s : prog.top_level_stmts) {
        unsupported(s.get(), "a statement outside any gene");
    }
    for (auto& g : prog.genes) {
        genGene(*g);
    }
    genEntryPoint();
    if (!errors.empty()) return false;
    out << ctx_.str(source_name_);
    return true;
}

void CodeGenerator::releaseIfOwned(const IRValue& v) {
    if (v.owned && isSequence(v.type)) {
        ctx_.declareExternal(RT_RELEASE);
        ctx_.emit("call void @gs_seq_release(ptr " + v.ref + ")");
    }
}

void CodeGenerator::releaseScopeVars(const std::vector<IRVariable>& vars) {
    for (const auto& v : vars) {
        if (!isSequence(v.type)) continue;
        std::string t = ctx_.newTemp();
        ctx_.emit(t + " = load ptr, ptr " + v.slot + ", align 8");
        ctx_.declareExternal(RT_RELEASE);
        ctx_.emit("call void @gs_seq_release(ptr " + t + ")");
    }
}

void CodeGenerator::genGene(GeneDeclAST& gene) {
    // ABI: every gene returns a sequence (`ptr`).  Semantics infers the type
    // from `express` statements and defaults to dna when nothing fixes it.
    auto sig = sem_.gene_signatures.find(gene.name);
    current_ret_ = (sig != sem_.gene_signatures.end() && isSequence(sig->second.return_type))
                       ? sig->second.return_type
                       : TYPE_DNA;

    std::vector<std::string> params;
    for (const auto& p : gene.params) {
        params.push_back(std::string(GS_SEQ_IR_TYPE) + " %p." + p.name);
    }
    ctx_.beginFunction(llvmType(current_ret_), geneSymbol(gene.name), params);
    ctx_.pushScope();
    loops_.clear();

    // ABI: parameters arrive BORROWED; each gets a local slot holding its
    // own (+1) reference, released when the gene returns.
    for (const auto& p : gene.params) {
        std::string slot = ctx_.emitAlloca("ptr", p.name);
        std::string t = ctx_.newTemp();
        ctx_.declareExternal(RT_RETAIN);
        ctx_.emit(t + " = call ptr @gs_seq_retain(ptr %p." + p.name + ")");
        ctx_.emit("store ptr " + t + ", ptr " + slot + ", align 8");
        ctx_.declareVar({p.name, slot, p.type});
    }

    for (auto& s : gene.body) genStmt(s.get());

    std::vector<IRVariable> vars = ctx_.popScope();
    if (!ctx_.blockTerminated()) {
        releaseScopeVars(vars);
        // Falling off the end of a gene yields "" (as in Phase 1).  The
        // literal is immortal, so returning it as an "owned" result is safe.
        ctx_.emitTerminator("ret ptr " + ctx_.internSequence(seqKind(current_ret_), ""));
    }
    ctx_.endFunction("unreachable");
}

void CodeGenerator::genEntryPoint() {
    if (sem_.gene_signatures.find("nucleus") == sem_.gene_signatures.end()) {
        return;  // semantics guarantees exactly one nucleus()
    }
    // C entry point: run nucleus(), drop its (owned) result, exit 0.
    ctx_.beginFunction("i32", "main", {});
    std::string t = ctx_.newTemp();
    ctx_.emit(t + " = call ptr @" + geneSymbol("nucleus") + "()");
    releaseIfOwned({t, TYPE_DNA, true});
    ctx_.emitTerminator("ret i32 0");
    ctx_.endFunction("unreachable");
}

std::string CodeGenerator::makeOwned(const IRValue& v) {
    if (v.owned) return v.ref;
    // Literals are immortal globals; retaining them would be a no-op call.
    if (!v.ref.empty() && v.ref[0] == '@') return v.ref;
    std::string t = ctx_.newTemp();
    ctx_.declareExternal(RT_RETAIN);
    ctx_.emit(t + " = call ptr @gs_seq_retain(ptr " + v.ref + ")");
    return t;
}

// ---- Statements -----------------------------------------------------------

void CodeGenerator::genStmt(StmtAST* stmt) {
    if (auto* decl = dynamic_cast<DeclStmtAST*>(stmt)) return genDecl(decl);
    if (auto* assign = dynamic_cast<AssignStmtAST*>(stmt)) return genAssign(assign);
    if (auto* rep = dynamic_cast<ReportStmtAST*>(stmt)) return genReport(rep);
    if (auto* exp = dynamic_cast<ExpressStmtAST*>(stmt)) return genExpress(exp);
    if (auto* w = dynamic_cast<WhenStmtAST*>(stmt)) return genWhen(w);
    if (auto* ca = dynamic_cast<CycleAcrossStmtAST*>(stmt)) return genCycleAcross(ca);
    if (auto* cft = dynamic_cast<CycleFromToStmtAST*>(stmt)) return genCycleFromTo(cft);
    if (auto* gr = dynamic_cast<GrowUntilStmtAST*>(stmt)) return genGrowUntil(gr);
    if (dynamic_cast<HaltStmtAST*>(stmt)) return genLoopExit(stmt, true);
    if (dynamic_cast<SkipStmtAST*>(stmt)) return genLoopExit(stmt, false);
    unsupported(stmt, stmtName(stmt));
}

// ---- Genes (Part 4) -----------------------------------------------------------

// express e: own the result FIRST (it may be a borrowed read of a variable
// that is about to be released), then release every live variable of the
// gene -- parameters, block locals and each enclosing `cycle across` loop's
// held sequence -- and return.  The caller receives an owned (+1) result.
void CodeGenerator::genExpress(ExpressStmtAST* exp) {
    IRValue v = genExpr(exp->expr.get());
    if (!isSequence(v.type)) {
        if (v.type != TYPE_UNKNOWN) unsupported(exp, "expressing a " + dataTypeToString(v.type) + " value");
        return;
    }
    std::string owned = makeOwned(v);
    releaseScopeVars(ctx_.liveVars());
    ctx_.emitTerminator("ret ptr " + owned);
}

// name(a1, a2, ...): arguments are evaluated left to right (as in Phase 1)
// and passed BORROWED; the callee retains what it keeps.  Owned argument
// temporaries are released after the call returns.  The result is owned.
// Genes may be defined anywhere in the file: LLVM resolves calls to
// functions defined later in the module, so recursion, mutual recursion and
// forward references need no declarations.
IRValue CodeGenerator::genCall(CallExprAST* call) {
    auto sig = sem_.gene_signatures.find(call->callee);
    if (sig == sem_.gene_signatures.end()) {  // semantics resolved every call
        unsupported(call, "call to unknown gene '" + call->callee + "'");
        return {"poison", TYPE_UNKNOWN, false};
    }
    std::vector<IRValue> args;
    for (auto& a : call->args) {
        IRValue v = genExpr(a.get());
        if (!isSequence(v.type)) return {"poison", TYPE_UNKNOWN, false};
        args.push_back(v);
    }
    std::string arg_text;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) arg_text += ", ";
        arg_text += "ptr " + args[i].ref;
    }
    DataType ret = isSequence(sig->second.return_type) ? sig->second.return_type : TYPE_DNA;
    std::string t = ctx_.newTemp();
    ctx_.emit(t + " = call ptr @" + geneSymbol(call->callee) + "(" + arg_text + ")");
    for (const auto& a : args) releaseIfOwned(a);
    return {t, ret, true};
}

// ---- Control flow (Part 3) --------------------------------------------------
//
// Every block (when branch, otherwise, loop body) is its own scope: sequence
// variables declared in it are released when the block falls through, and by
// halt/skip when they jump out of it.  Labels come from IRContext::newLabel,
// so they are unique within the function (when.then.3, across.cond.7, ...).

void CodeGenerator::genBlock(const std::vector<std::unique_ptr<StmtAST>>& body) {
    ctx_.pushScope();
    for (auto& s : body) genStmt(s.get());
    std::vector<IRVariable> vars = ctx_.popScope();
    if (!ctx_.blockTerminated()) releaseScopeVars(vars);
}

std::string CodeGenerator::genCondition(ExprAST* cond) {
    IRValue c = genExpr(cond);
    if (c.type == TYPE_BOOL) return c.ref;
    if (c.type != TYPE_UNKNOWN) {  // semantics rejects this; never emit bad IR
        unsupported(cond, "a " + dataTypeToString(c.type) + " value used as a condition");
    }
    return "";
}

// when c1 ... otherwise when c2 ... otherwise ... stop
//
//     br i1 %c1, label %when.then.A, label %when.else.B
//   when.then.A:  <body 1>                 br label %when.end.E
//   when.else.B:  %c2 = ...   br i1 %c2, label %when.then.C, label %when.else.D
//   when.then.C:  <body 2>                 br label %when.end.E
//   when.else.D:  <otherwise body>         br label %when.end.E
//   when.end.E:
//
// Conditions are evaluated in order and only until one is true, as in Phase 1.
void CodeGenerator::genWhen(WhenStmtAST* w) {
    std::string end = ctx_.newLabel("when.end");
    const size_t n = w->branches.size();
    for (size_t k = 0; k < n; ++k) {
        std::string c = genCondition(w->branches[k].condition.get());
        if (c.empty()) return;
        bool last = (k + 1 == n);
        std::string then = ctx_.newLabel("when.then");
        std::string next = (last && w->otherwise_body.empty()) ? end : ctx_.newLabel("when.else");
        ctx_.emitTerminator("br i1 " + c + ", label %" + then + ", label %" + next);

        ctx_.startBlock(then);
        genBlock(w->branches[k].body);
        if (!ctx_.blockTerminated()) ctx_.emitTerminator("br label %" + end);
        if (next != end) ctx_.startBlock(next);
    }
    if (!w->otherwise_body.empty()) {
        genBlock(w->otherwise_body);
        if (!ctx_.blockTerminated()) ctx_.emitTerminator("br label %" + end);
    }
    ctx_.startBlock(end);
}

void CodeGenerator::genLoopBody(const std::vector<std::unique_ptr<StmtAST>>& body,
                                const std::string& brk, const std::string& cont) {
    loops_.push_back({brk, cont, ctx_.scopeDepth()});
    genBlock(body);
    loops_.pop_back();
    if (!ctx_.blockTerminated()) ctx_.emitTerminator("br label %" + cont);
}

// The counter lives in an i64 stack slot, declared in a loop scope that
// encloses the body.  Semantics makes it read-only, so the slot doubles as the
// loop index.  mem2reg turns it into a phi at -O1 and above.
static std::string emitCounterLoad(IRContext& ctx, const std::string& slot) {
    std::string t = ctx.newTemp();
    ctx.emit(t + " = load i64, ptr " + slot + ", align 8");
    return t;
}

static void emitCounterStep(IRContext& ctx, const std::string& slot) {
    std::string i = emitCounterLoad(ctx, slot);
    std::string next = ctx.newTemp();
    ctx.emit(next + " = add nsw i64 " + i + ", 1");
    ctx.emit("store i64 " + next + ", ptr " + slot + ", align 8");
}

// cycle i across s ... stop
//
// As in Phase 1, s is evaluated once before the loop and the loop visits
// positions 0 .. len(s)-1, even if the body reassigns the variable s.  The
// loop keeps its own reference to that first value (hidden variable), so the
// body cannot free it; the reference is released after the loop.
//
//     %s = <s>, retained;  %len = call i64 @gs_seq_len(ptr %s);  i = 0
//     br label %across.cond.A
//   across.cond.A:  %i = load i;  %c = icmp slt i64 %i, %len
//                   br i1 %c, label %across.body.B, label %across.end.D
//   across.body.B:  <body>                         br label %across.step.C
//   across.step.C:  i = i + 1                      br label %across.cond.A
//   across.end.D:   release %s
//
// halt -> across.end, skip -> across.step.
void CodeGenerator::genCycleAcross(CycleAcrossStmtAST* loop) {
    IRValue s = genExpr(loop->seq_expr.get());
    if (!isSequence(s.type)) return;

    ctx_.pushScope();  // loop scope: the held sequence and the counter
    std::string held = makeOwned(s);
    std::string seq_slot = ctx_.emitAlloca("ptr", "across", "null");
    ctx_.emit("store ptr " + held + ", ptr " + seq_slot + ", align 8");
    ctx_.declareVar({" across", seq_slot, s.type});  // space: not a GeneScript name

    std::string len = ctx_.newTemp();
    ctx_.declareExternal(RT_LEN);
    ctx_.emit(len + " = call i64 @gs_seq_len(ptr " + held + ")");
    std::string ctr = ctx_.emitAlloca("i64", loop->counter_var);
    ctx_.emit("store i64 0, ptr " + ctr + ", align 8");
    ctx_.declareVar({loop->counter_var, ctr, TYPE_INT});

    std::string cond = ctx_.newLabel("across.cond");
    std::string body = ctx_.newLabel("across.body");
    std::string step = ctx_.newLabel("across.step");
    std::string end = ctx_.newLabel("across.end");

    ctx_.startBlock(cond);
    std::string i = emitCounterLoad(ctx_, ctr);
    std::string more = ctx_.newTemp();
    ctx_.emit(more + " = icmp slt i64 " + i + ", " + len);
    ctx_.emitTerminator("br i1 " + more + ", label %" + body + ", label %" + end);

    ctx_.startBlock(body);
    genLoopBody(loop->body, end, step);

    ctx_.startBlock(step);
    emitCounterStep(ctx_, ctr);
    ctx_.emitTerminator("br label %" + cond);

    ctx_.startBlock(end);
    releaseScopeVars(ctx_.popScope());
}

// cycle i from a to b ... stop
//
// a and b are evaluated once, before the loop (Phase 1).  The loop runs
// i = a, a+1, ..., b-1; it runs zero times when a >= b.
//
//     %a = <a>;  %b = <b>;  i = %a
//     br label %count.cond.A
//   count.cond.A:  %i = load i;  %c = icmp slt i64 %i, %b
//                  br i1 %c, label %count.body.B, label %count.end.D
//   count.body.B:  <body>                          br label %count.step.C
//   count.step.C:  i = i + 1                       br label %count.cond.A
//   count.end.D:
void CodeGenerator::genCycleFromTo(CycleFromToStmtAST* loop) {
    IRValue from = genExpr(loop->from_expr.get());
    IRValue to = genExpr(loop->to_expr.get());
    if (from.type != TYPE_INT || to.type != TYPE_INT) return;  // error recorded

    ctx_.pushScope();  // loop scope: the counter
    std::string ctr = ctx_.emitAlloca("i64", loop->counter_var);
    ctx_.emit("store i64 " + from.ref + ", ptr " + ctr + ", align 8");
    ctx_.declareVar({loop->counter_var, ctr, TYPE_INT});

    std::string cond = ctx_.newLabel("count.cond");
    std::string body = ctx_.newLabel("count.body");
    std::string step = ctx_.newLabel("count.step");
    std::string end = ctx_.newLabel("count.end");

    ctx_.startBlock(cond);
    std::string i = emitCounterLoad(ctx_, ctr);
    std::string more = ctx_.newTemp();
    ctx_.emit(more + " = icmp slt i64 " + i + ", " + to.ref);
    ctx_.emitTerminator("br i1 " + more + ", label %" + body + ", label %" + end);

    ctx_.startBlock(body);
    genLoopBody(loop->body, end, step);

    ctx_.startBlock(step);
    emitCounterStep(ctx_, ctr);
    ctx_.emitTerminator("br label %" + cond);

    ctx_.startBlock(end);
    releaseScopeVars(ctx_.popScope());
}

// grow until c ... stop
//
// The condition is tested before every pass; the body runs while it is
// still false and the loop ends as soon as it becomes true (Phase 1).
//
//     br label %grow.cond.A
//   grow.cond.A:  %c = <c>   br i1 %c, label %grow.end.C, label %grow.body.B
//   grow.body.B:  <body>                           br label %grow.cond.A
//   grow.end.C:
//
// halt -> grow.end, skip -> grow.cond.
void CodeGenerator::genGrowUntil(GrowUntilStmtAST* loop) {
    std::string cond = ctx_.newLabel("grow.cond");
    std::string body = ctx_.newLabel("grow.body");
    std::string end = ctx_.newLabel("grow.end");

    ctx_.startBlock(cond);
    std::string c = genCondition(loop->cond_expr.get());
    if (c.empty()) return;
    ctx_.emitTerminator("br i1 " + c + ", label %" + end + ", label %" + body);

    ctx_.startBlock(body);
    genLoopBody(loop->body, end, cond);

    ctx_.startBlock(end);
}

// halt / skip: release every sequence variable declared inside the innermost
// loop's body (including nested when blocks) so far, then jump.
void CodeGenerator::genLoopExit(StmtAST* stmt, bool is_halt) {
    if (loops_.empty()) {  // semantics rejects this; never emit a stray br
        unsupported(stmt, std::string(is_halt ? "'halt'" : "'skip'") + " outside a loop");
        return;
    }
    const LoopTargets& loop = loops_.back();
    releaseScopeVars(ctx_.varsFrom(loop.body_scope_depth));
    ctx_.emitTerminator("br label %" + (is_halt ? loop.break_label : loop.continue_label));
}

// both / either with short-circuit evaluation (the right side runs only when
// it can change the result, as in Phase 1):
//
//     %l = <lhs>
//     br i1 %l, label %both.rhs.A, label %both.end.B     ; either: swapped
//   both.rhs.A:  %r = <rhs>                         br label %both.end.B
//   both.end.B:  %v = phi i1 [ false, %<lhs block> ], [ %r, %<rhs block> ]
IRValue CodeGenerator::genLogical(BinaryExprAST* bin) {
    const bool is_both = (bin->op == OP_BOTH);
    const std::string word = is_both ? "both" : "either";

    std::string l = genCondition(bin->lhs.get());
    if (l.empty()) return {"poison", TYPE_UNKNOWN, false};
    std::string lhs_block = ctx_.currentBlock();
    std::string rhs = ctx_.newLabel(word + ".rhs");
    std::string end = ctx_.newLabel(word + ".end");
    if (is_both) {
        ctx_.emitTerminator("br i1 " + l + ", label %" + rhs + ", label %" + end);
    } else {
        ctx_.emitTerminator("br i1 " + l + ", label %" + end + ", label %" + rhs);
    }

    ctx_.startBlock(rhs);
    std::string r = genCondition(bin->rhs.get());
    if (r.empty()) return {"poison", TYPE_UNKNOWN, false};
    std::string rhs_block = ctx_.currentBlock();  // rhs may itself add blocks
    ctx_.emitTerminator("br label %" + end);

    ctx_.startBlock(end);
    std::string v = ctx_.newTemp();
    ctx_.emit(v + " = phi i1 [ " + std::string(is_both ? "false" : "true") + ", %" + lhs_block +
              " ], [ " + r + ", %" + rhs_block + " ]");
    return {v, TYPE_BOOL, false};
}

// "e -> dna x": the new slot owns +1 reference to the value.
void CodeGenerator::genDecl(DeclStmtAST* decl) {
    IRValue v = genExpr(decl->init_expr.get());
    if (!isSequence(v.type)) return;  // error already recorded
    std::string owned = makeOwned(v);
    std::string slot = ctx_.emitAlloca("ptr", decl->var_name, "null");
    ctx_.emit("store ptr " + owned + ", ptr " + slot + ", align 8");
    ctx_.declareVar({decl->var_name, slot, decl->declared_type});
}

// "e -> x": own the new value FIRST, then drop the old one, so that
// `tail(x) -> x` and `x -> x` never touch a freed sequence.
void CodeGenerator::genAssign(AssignStmtAST* assign) {
    const IRVariable* var = ctx_.lookupVar(assign->var_name);
    if (!var || !isSequence(var->type)) {  // semantics forbids assigning counters
        unsupported(assign, "assignment to '" + assign->var_name + "'");
        return;
    }
    std::string slot = var->slot;
    IRValue v = genExpr(assign->expr.get());
    if (!isSequence(v.type)) return;
    std::string owned = makeOwned(v);
    std::string old = ctx_.newTemp();
    ctx_.emit(old + " = load ptr, ptr " + slot + ", align 8");
    ctx_.declareExternal(RT_RELEASE);
    ctx_.emit("call void @gs_seq_release(ptr " + old + ")");
    ctx_.emit("store ptr " + owned + ", ptr " + slot + ", align 8");
}

void CodeGenerator::genReport(ReportStmtAST* rep) {
    IRValue v = genExpr(rep->expr.get());
    if (v.type == TYPE_UNKNOWN) return;  // error already recorded
    ctx_.declareExternal(RT_REPORT);
    if (isSequence(v.type)) {
        ctx_.emit("call void @gs_report(ptr " + v.ref + ")");  // borrows v
        releaseIfOwned(v);
    } else if (v.type == TYPE_BOOL) {
        // Phase 1 compatibility: semantics accepts `report <condition>` and
        // the interpreter prints an empty line (Value::str_val of a boolean).
        // The condition is still evaluated for its runtime checks.
        ctx_.emit("call void @gs_report(ptr " + ctx_.internSequence(GS_KIND_DNA, "") + ")");
    } else {
        unsupported(rep, "reporting a " + dataTypeToString(v.type) + " value");
    }
}

// ---- Expressions ----------------------------------------------------------

IRValue CodeGenerator::genExpr(ExprAST* expr) {
    if (auto* lit = dynamic_cast<LiteralExprAST*>(expr)) {
        // Semantics fixed the literal's type from context; default is DNA.
        DataType t = isSequence(lit->deduced_type) ? lit->deduced_type : TYPE_DNA;
        // Literals are immortal globals: a BORROWED reference that is valid
        // forever, so it never needs releasing.
        return {ctx_.internSequence(seqKind(t), lit->value), t, false};
    }
    if (auto* ilit = dynamic_cast<IntLiteralExprAST*>(expr)) {
        return {std::to_string(ilit->value), TYPE_INT, false};
    }
    if (auto* blit = dynamic_cast<BoolLiteralExprAST*>(expr)) {
        return {blit->value ? "true" : "false", TYPE_BOOL, false};
    }
    if (auto* var = dynamic_cast<VarExprAST*>(expr)) return genVar(var);
    if (auto* call = dynamic_cast<CallExprAST*>(expr)) return genCall(call);
    if (auto* bc = dynamic_cast<BuiltinCallExprAST*>(expr)) {
        switch (bc->op) {
            case BUILTIN_HEAD: return genSeqUnary(bc->arg.get(), RT_HEAD, TYPE_UNKNOWN);
            case BUILTIN_TAIL: return genSeqUnary(bc->arg.get(), RT_TAIL, TYPE_UNKNOWN);
            case BUILTIN_TRANSCRIBE: return genSeqUnary(bc->arg.get(), RT_TRANSCRIBE, TYPE_RNA);
        }
    }
    if (auto* un = dynamic_cast<UnaryExprAST*>(expr)) {
        if (un->op == OP_REVCOMP) return genSeqUnary(un->operand.get(), RT_REVCOMP, TYPE_DNA);
        if (un->op == OP_LEN) {
            // Only reachable from cycle bounds (Part 3); semantics rejects #s
            // everywhere else.
            IRValue s = genExpr(un->operand.get());
            if (!isSequence(s.type)) return {"poison", TYPE_UNKNOWN, false};
            std::string t = ctx_.newTemp();
            ctx_.declareExternal(RT_LEN);
            ctx_.emit(t + " = call i64 @gs_seq_len(ptr " + s.ref + ")");
            releaseIfOwned(s);
            return {t, TYPE_INT, false};
        }
        if (un->op == OP_LACKS) {
            std::string c = genCondition(un->operand.get());
            if (c.empty()) return {"poison", TYPE_UNKNOWN, false};
            std::string t = ctx_.newTemp();
            ctx_.emit(t + " = xor i1 " + c + ", true");
            return {t, TYPE_BOOL, false};
        }
    }
    if (auto* idx = dynamic_cast<IndexExprAST*>(expr)) {
        IRValue s = genExpr(idx->seq.get());
        IRValue i = genExpr(idx->index.get());
        if (!isSequence(s.type) || i.type != TYPE_INT) return {"poison", TYPE_UNKNOWN, false};
        std::string t = ctx_.newTemp();
        ctx_.declareExternal(RT_AT);
        ctx_.emit(t + " = call ptr @gs_seq_at(ptr " + s.ref + ", i64 " + i.ref + ")");
        releaseIfOwned(s);
        return {t, s.type, true};
    }
    if (auto* bin = dynamic_cast<BinaryExprAST*>(expr)) {
        if (bin->op != OP_BOTH && bin->op != OP_EITHER) return genBinary(bin);
        return genLogical(bin);
    }
    unsupported(expr, exprName(expr));
    return {"poison", TYPE_UNKNOWN, false};
}

IRValue CodeGenerator::genVar(VarExprAST* var) {
    const IRVariable* v = ctx_.lookupVar(var->name);
    if (!v) {  // semantics resolved every name; never emit a dangling load
        unsupported(var, "reading '" + var->name + "'");
        return {"poison", TYPE_UNKNOWN, false};
    }
    std::string t = ctx_.newTemp();
    if (v->type == TYPE_INT) {  // loop counter: a plain i64, no ownership
        ctx_.emit(t + " = load i64, ptr " + v->slot + ", align 8");
        return {t, TYPE_INT, false};
    }
    ctx_.emit(t + " = load ptr, ptr " + v->slot + ", align 8");
    // BORROWED: valid until the variable is next assigned.  Expressions
    // cannot assign variables, so it outlives the expression using it.
    return {t, v->type, false};
}

IRValue CodeGenerator::genSeqUnary(ExprAST* operand, const char* fn, DataType result_type) {
    IRValue a = genExpr(operand);
    if (!isSequence(a.type)) return {"poison", TYPE_UNKNOWN, false};
    std::string t = ctx_.newTemp();
    ctx_.declareExternal(std::string("ptr @") + fn + "(ptr)");
    ctx_.emit(t + " = call ptr @" + fn + "(ptr " + a.ref + ")");
    releaseIfOwned(a);
    return {t, result_type == TYPE_UNKNOWN ? a.type : result_type, true};
}

IRValue CodeGenerator::genBinary(BinaryExprAST* bin) {
    // Left operand first, as in the interpreter (matters for runtime errors).
    IRValue l = genExpr(bin->lhs.get());
    IRValue r = genExpr(bin->rhs.get());
    if (!isSequence(l.type) || !isSequence(r.type)) return {"poison", TYPE_UNKNOWN, false};

    IRValue result;
    std::string t = ctx_.newTemp();
    if (bin->op == OP_FUSE) {
        ctx_.declareExternal(RT_FUSE);
        ctx_.emit(t + " = call ptr @gs_seq_fuse(ptr " + l.ref + ", ptr " + r.ref + ")");
        DataType ty = isSequence(bin->deduced_type) ? bin->deduced_type : l.type;
        result = {t, ty, true};
    } else if (bin->op == OP_EQ) {
        ctx_.declareExternal(RT_EQUAL);
        ctx_.emit(t + " = call i32 @gs_seq_equal(ptr " + l.ref + ", ptr " + r.ref + ")");
        std::string b = ctx_.newTemp();
        ctx_.emit(b + " = icmp ne i32 " + t + ", 0");
        result = {b, TYPE_BOOL, false};
    } else {
        // <, >, <=, >= on single bases; the runtime enforces "single base"
        // for operands semantics could not check (variables).
        const char* pred = bin->op == OP_LT ? "slt" : bin->op == OP_GT ? "sgt"
                         : bin->op == OP_LE ? "sle" : "sge";
        ctx_.declareExternal(RT_CMP_BASES);
        ctx_.emit(t + " = call i32 @gs_seq_compare_bases(ptr " + l.ref + ", ptr " + r.ref +
                  ", i64 " + std::to_string(bin->loc.line) + ")");
        std::string b = ctx_.newTemp();
        ctx_.emit(b + " = icmp " + pred + " i32 " + t + ", 0");
        result = {b, TYPE_BOOL, false};
    }
    releaseIfOwned(l);
    releaseIfOwned(r);
    return result;
}
