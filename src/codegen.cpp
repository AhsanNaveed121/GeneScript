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
    unsupported(stmt, stmtName(stmt));
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
    if (!var) {  // e.g. a loop counter (Part 3); semantics checked the rest
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
        // `lacks` belongs with conditions (Part 3).
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
        // both/either need short-circuit branches: Part 3 with `when`.
    }
    unsupported(expr, exprName(expr));
    return {"poison", TYPE_UNKNOWN, false};
}

IRValue CodeGenerator::genVar(VarExprAST* var) {
    const IRVariable* v = ctx_.lookupVar(var->name);
    if (!v) {  // loop counters are not lowered yet (Part 3)
        unsupported(var, "reading '" + var->name + "' (loop counter)");
        return {"poison", TYPE_UNKNOWN, false};
    }
    std::string t = ctx_.newTemp();
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
