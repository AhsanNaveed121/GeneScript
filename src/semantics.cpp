#include "semantics.h"
#include <iostream>
#include <sstream>

std::string dataTypeToString(DataType t) {
    switch (t) {
        case TYPE_DNA: return "dna";
        case TYPE_RNA: return "rna";
        case TYPE_INT: return "integer";
        case TYPE_BOOL: return "boolean";
        case TYPE_VOID: return "void";
        default: return "unknown";
    }
}

bool SemanticAnalyzer::isValidDNA(const std::string& s, char& invalid_char) {
    for (char c : s) {
        if (c != 'A' && c != 'C' && c != 'G' && c != 'T') {
            invalid_char = c;
            return false;
        }
    }
    return true;
}

bool SemanticAnalyzer::isValidRNA(const std::string& s, char& invalid_char) {
    for (char c : s) {
        if (c != 'A' && c != 'C' && c != 'G' && c != 'U') {
            invalid_char = c;
            return false;
        }
    }
    return true;
}

void SemanticAnalyzer::reportError(SourceLoc loc, const std::string& msg, const std::string& note) {
    std::ostringstream oss;
    oss << filename << ":" << loc.line << ": " << msg << "\n";
    if (!note.empty()) {
        oss << "              " << note << "\n";
    }
    errors.push_back(oss.str());
}

DataType SemanticAnalyzer::validateLiteral(LiteralExprAST* lit, DataType expected_context_type) {
    const std::string& s = lit->value;
    bool has_T = (s.find('T') != std::string::npos);
    bool has_U = (s.find('U') != std::string::npos);

    if (has_T && has_U) {
        reportError(lit->loc, "mixed DNA and RNA bases in literal.",
                    "DNA uses T and RNA uses U; they cannot be mixed.");
        return TYPE_UNKNOWN;
    }

    DataType actual_type = TYPE_UNKNOWN;
    if (has_T) {
        actual_type = TYPE_DNA;
    } else if (has_U) {
        actual_type = TYPE_RNA;
    } else if (expected_context_type == TYPE_DNA || expected_context_type == TYPE_RNA) {
        actual_type = expected_context_type;
    } else {
        // Default to DNA if ambiguous
        actual_type = TYPE_DNA;
    }

    char bad_c = 0;
    if (expected_context_type == TYPE_DNA || (expected_context_type == TYPE_UNKNOWN && actual_type == TYPE_DNA)) {
        if (!isValidDNA(s, bad_c)) {
            std::string note = "DNA allows only A, C, G, T.";
            if (bad_c == 'U') {
                note = "base 'U' is RNA only; DNA allows only A, C, G, T.";
            }
            reportError(lit->loc, std::string("invalid base '") + bad_c + "' in DNA literal.", note);
            return TYPE_UNKNOWN;
        }
        actual_type = TYPE_DNA;
    } else if (expected_context_type == TYPE_RNA || (expected_context_type == TYPE_UNKNOWN && actual_type == TYPE_RNA)) {
        if (!isValidRNA(s, bad_c)) {
            std::string note = "RNA allows only A, C, G, U.";
            if (bad_c == 'T') {
                note = "base 'T' is DNA only; RNA allows only A, C, G, U.";
            }
            reportError(lit->loc, std::string("invalid base '") + bad_c + "' in RNA literal.", note);
            return TYPE_UNKNOWN;
        }
        actual_type = TYPE_RNA;
    }

    lit->deduced_type = actual_type;
    return actual_type;
}

DataType SemanticAnalyzer::checkExpr(ExprAST* expr, std::shared_ptr<Scope> scope, DataType expected) {
    if (!expr) return TYPE_UNKNOWN;

    if (auto* lit = dynamic_cast<LiteralExprAST*>(expr)) {
        return validateLiteral(lit, expected);
    }
    if (auto* ilit = dynamic_cast<IntLiteralExprAST*>(expr)) {
        ilit->deduced_type = TYPE_INT;
        return TYPE_INT;
    }
    if (auto* blit = dynamic_cast<BoolLiteralExprAST*>(expr)) {
        blit->deduced_type = TYPE_BOOL;
        return TYPE_BOOL;
    }
    if (auto* var = dynamic_cast<VarExprAST*>(expr)) {
        Symbol* sym = scope->lookup(var->name);
        if (!sym) {
            reportError(var->loc, "undeclared variable or sequence '" + var->name + "'.");
            return TYPE_UNKNOWN;
        }
        var->deduced_type = sym->type;
        return sym->type;
    }
    if (auto* bin = dynamic_cast<BinaryExprAST*>(expr)) {
        if (bin->op == OP_FUSE) {
            DataType t1 = checkExpr(bin->lhs.get(), scope, expected);
            DataType t2 = checkExpr(bin->rhs.get(), scope, t1 != TYPE_UNKNOWN ? t1 : expected);
            if (t1 == TYPE_UNKNOWN && t2 != TYPE_UNKNOWN) {
                // re-check lhs with context
                t1 = checkExpr(bin->lhs.get(), scope, t2);
            }
            if (t1 != TYPE_UNKNOWN && t2 != TYPE_UNKNOWN && t1 != t2) {
                reportError(bin->loc, "cannot fuse " + dataTypeToString(t1) + " with " + dataTypeToString(t2) + ".",
                            "fuse requires both sequences to have the same biological type.");
                return TYPE_UNKNOWN;
            }
            bin->deduced_type = (t1 != TYPE_UNKNOWN) ? t1 : t2;
            return bin->deduced_type;
        }
        if (bin->op == OP_EQ) {
            DataType t1 = checkExpr(bin->lhs.get(), scope, TYPE_UNKNOWN);
            DataType t2 = checkExpr(bin->rhs.get(), scope, t1);
            if (t1 == TYPE_UNKNOWN && t2 != TYPE_UNKNOWN) {
                t1 = checkExpr(bin->lhs.get(), scope, t2);
            }
            if (t1 != TYPE_UNKNOWN && t2 != TYPE_UNKNOWN && t1 != t2) {
                reportError(bin->loc, "cannot compare " + dataTypeToString(t1) + " with " + dataTypeToString(t2) + " using '='.",
                            "comparison '=' requires sequences of the same type.");
            }
            bin->deduced_type = TYPE_BOOL;
            return TYPE_BOOL;
        }
        if (bin->op == OP_LT || bin->op == OP_GT || bin->op == OP_LE || bin->op == OP_GE) {
            DataType t1 = checkExpr(bin->lhs.get(), scope, TYPE_UNKNOWN);
            DataType t2 = checkExpr(bin->rhs.get(), scope, t1);
            if (t1 != TYPE_UNKNOWN && t2 != TYPE_UNKNOWN && t1 != t2) {
                reportError(bin->loc, "cannot compare " + dataTypeToString(t1) + " with " + dataTypeToString(t2) + ".",
                            "ordering comparisons (<, >, <=, >=) compare bases of the same type.");
            }
            bin->deduced_type = TYPE_BOOL;
            return TYPE_BOOL;
        }
        if (bin->op == OP_BOTH || bin->op == OP_EITHER) {
            checkExpr(bin->lhs.get(), scope, TYPE_BOOL);
            checkExpr(bin->rhs.get(), scope, TYPE_BOOL);
            bin->deduced_type = TYPE_BOOL;
            return TYPE_BOOL;
        }
    }
    if (auto* un = dynamic_cast<UnaryExprAST*>(expr)) {
        if (un->op == OP_REVCOMP) {
            DataType t = checkExpr(un->operand.get(), scope, TYPE_DNA);
            if (t != TYPE_DNA && t != TYPE_UNKNOWN) {
                reportError(un->loc, "reverse complement (~) is only supported for DNA.",
                            "RNA reverse complement is outside biological scope.");
            }
            un->deduced_type = TYPE_DNA;
            return TYPE_DNA;
        }
        if (un->op == OP_LACKS) {
            checkExpr(un->operand.get(), scope, TYPE_BOOL);
            un->deduced_type = TYPE_BOOL;
            return TYPE_BOOL;
        }
        if (un->op == OP_LEN) {
            checkExpr(un->operand.get(), scope, TYPE_UNKNOWN);
            un->deduced_type = TYPE_INT;
            return TYPE_INT;
        }
    }
    if (auto* bc = dynamic_cast<BuiltinCallExprAST*>(expr)) {
        if (bc->op == BUILTIN_HEAD || bc->op == BUILTIN_TAIL) {
            DataType t = checkExpr(bc->arg.get(), scope, expected);
            bc->deduced_type = t;
            return t;
        }
        if (bc->op == BUILTIN_TRANSCRIBE) {
            DataType t = checkExpr(bc->arg.get(), scope, TYPE_DNA);
            if (t != TYPE_DNA && t != TYPE_UNKNOWN) {
                reportError(bc->loc, "transcribe() requires DNA sequence as argument.",
                            "Transcription converts DNA into RNA.");
            }
            bc->deduced_type = TYPE_RNA;
            return TYPE_RNA;
        }
    }
    if (auto* idx = dynamic_cast<IndexExprAST*>(expr)) {
        DataType tseq = checkExpr(idx->seq.get(), scope, expected);
        DataType tidx = checkExpr(idx->index.get(), scope, TYPE_INT);
        if (tidx != TYPE_INT && tidx != TYPE_UNKNOWN) {
            reportError(idx->loc, "index in sequence indexing (@) must be an integer.");
        }
        idx->deduced_type = tseq;
        return tseq;
    }
    if (auto* call = dynamic_cast<CallExprAST*>(expr)) {
        auto it = gene_signatures.find(call->callee);
        if (it == gene_signatures.end()) {
            reportError(call->loc, "call to undefined gene '" + call->callee + "'.");
            return TYPE_UNKNOWN;
        }
        const auto& sig = it->second;
        if (sig.param_types.size() != call->args.size()) {
            reportError(call->loc, "gene '" + call->callee + "' expects " +
                        std::to_string(sig.param_types.size()) + " arguments, but got " +
                        std::to_string(call->args.size()) + ".");
        } else {
            for (size_t i = 0; i < call->args.size(); ++i) {
                checkExpr(call->args[i].get(), scope, sig.param_types[i]);
            }
        }
        call->deduced_type = sig.return_type;
        return sig.return_type;
    }

    return TYPE_UNKNOWN;
}

void SemanticAnalyzer::checkStmt(StmtAST* stmt, std::shared_ptr<Scope> scope, DataType enclosing_gene_ret, int loop_depth) {
    if (!stmt) return;

    if (auto* decl = dynamic_cast<DeclStmtAST*>(stmt)) {
        DataType init_type = checkExpr(decl->init_expr.get(), scope, decl->declared_type);
        if (init_type != TYPE_UNKNOWN && init_type != decl->declared_type) {
            reportError(decl->loc, "cannot assign " + dataTypeToString(init_type) + " to variable '" +
                        decl->var_name + "' of type " + dataTypeToString(decl->declared_type) + ".");
        }
        Symbol sym;
        sym.name = decl->var_name;
        sym.type = decl->declared_type;
        sym.is_counter = false;
        sym.loc = decl->loc;
        if (!scope->declare(sym)) {
            reportError(decl->loc, "redeclaration of variable '" + decl->var_name + "' in the same scope.");
        }
        return;
    }

    if (auto* assign = dynamic_cast<AssignStmtAST*>(stmt)) {
        Symbol* sym = scope->lookup(assign->var_name);
        if (!sym) {
            reportError(assign->loc, "assignment to undeclared variable '" + assign->var_name + "'.");
            return;
        }
        if (sym->is_counter) {
            reportError(assign->loc, "cannot assign to loop counter '" + assign->var_name + "'.",
                        "Loop counters are read-only.");
            return;
        }
        DataType val_type = checkExpr(assign->expr.get(), scope, sym->type);
        if (val_type != TYPE_UNKNOWN && val_type != sym->type) {
            reportError(assign->loc, "cannot assign " + dataTypeToString(val_type) + " to variable '" +
                        assign->var_name + "' of type " + dataTypeToString(sym->type) + ".");
        }
        return;
    }

    if (auto* rep = dynamic_cast<ReportStmtAST*>(stmt)) {
        DataType t = checkExpr(rep->expr.get(), scope, TYPE_UNKNOWN);
        if (t == TYPE_INT) {
            reportError(rep->loc, "integers cannot be reported.",
                        "GeneScript restricts output to biological sequences (DNA/RNA).");
        }
        return;
    }

    if (auto* exp = dynamic_cast<ExpressStmtAST*>(stmt)) {
        DataType t = checkExpr(exp->expr.get(), scope, enclosing_gene_ret);
        if (enclosing_gene_ret != TYPE_UNKNOWN && t != TYPE_UNKNOWN && t != enclosing_gene_ret) {
            reportError(exp->loc, "gene return type mismatch: returning " + dataTypeToString(t) +
                        " but expected " + dataTypeToString(enclosing_gene_ret) + ".");
        }
        return;
    }

    if (auto* w = dynamic_cast<WhenStmtAST*>(stmt)) {
        for (auto& b : w->branches) {
            checkExpr(b.condition.get(), scope, TYPE_BOOL);
            auto branch_scope = std::make_shared<Scope>(scope);
            for (auto& s : b.body) {
                checkStmt(s.get(), branch_scope, enclosing_gene_ret, loop_depth);
            }
        }
        if (!w->otherwise_body.empty()) {
            auto oth_scope = std::make_shared<Scope>(scope);
            for (auto& s : w->otherwise_body) {
                checkStmt(s.get(), oth_scope, enclosing_gene_ret, loop_depth);
            }
        }
        return;
    }

    if (auto* cyc = dynamic_cast<CycleAcrossStmtAST*>(stmt)) {
        checkExpr(cyc->seq_expr.get(), scope, TYPE_UNKNOWN);
        auto loop_scope = std::make_shared<Scope>(scope);
        Symbol counter_sym;
        counter_sym.name = cyc->counter_var;
        counter_sym.type = TYPE_INT; // counter index (0 to len-1)
        counter_sym.is_counter = true;
        counter_sym.loc = cyc->loc;
        loop_scope->declare(counter_sym);
        for (auto& s : cyc->body) {
            checkStmt(s.get(), loop_scope, enclosing_gene_ret, loop_depth + 1);
        }
        return;
    }

    if (auto* cft = dynamic_cast<CycleFromToStmtAST*>(stmt)) {
        checkExpr(cft->from_expr.get(), scope, TYPE_INT);
        checkExpr(cft->to_expr.get(), scope, TYPE_INT);
        auto loop_scope = std::make_shared<Scope>(scope);
        Symbol counter_sym;
        counter_sym.name = cft->counter_var;
        counter_sym.type = TYPE_INT;
        counter_sym.is_counter = true;
        counter_sym.loc = cft->loc;
        loop_scope->declare(counter_sym);
        for (auto& s : cft->body) {
            checkStmt(s.get(), loop_scope, enclosing_gene_ret, loop_depth + 1);
        }
        return;
    }

    if (auto* gr = dynamic_cast<GrowUntilStmtAST*>(stmt)) {
        checkExpr(gr->cond_expr.get(), scope, TYPE_BOOL);
        auto loop_scope = std::make_shared<Scope>(scope);
        for (auto& s : gr->body) {
            checkStmt(s.get(), loop_scope, enclosing_gene_ret, loop_depth + 1);
        }
        return;
    }

    if (dynamic_cast<HaltStmtAST*>(stmt)) {
        if (loop_depth == 0) {
            reportError(stmt->loc, "halt (break) statement outside of any loop.");
        }
        return;
    }

    if (dynamic_cast<SkipStmtAST*>(stmt)) {
        if (loop_depth == 0) {
            reportError(stmt->loc, "skip (continue) statement outside of any loop.");
        }
        return;
    }
}

// First pass: collect gene signatures to allow forward calls & mutual recursion
void collectSignatures(ProgramAST& prog, SemanticAnalyzer& sem) {
    for (auto& gene : prog.genes) {
        GeneSignature sig;
        sig.name = gene->name;
        sig.loc = gene->loc;
        for (const auto& p : gene->params) {
            sig.param_types.push_back(p.type);
        }
        // Infer return type from express statements in gene body
        sig.return_type = TYPE_UNKNOWN;
        for (const auto& stmt : gene->body) {
            if (auto* exp = dynamic_cast<ExpressStmtAST*>(stmt.get())) {
                if (auto* lit = dynamic_cast<LiteralExprAST*>(exp->expr.get())) {
                    if (lit->value.find('U') != std::string::npos) sig.return_type = TYPE_RNA;
                    else sig.return_type = TYPE_DNA;
                }
            }
        }
        if (sig.return_type == TYPE_UNKNOWN) {
            sig.return_type = TYPE_DNA; // default
        }
        sem.gene_signatures[gene->name] = sig;
    }
}

bool SemanticAnalyzer::analyze(ProgramAST& prog) {
    collectSignatures(prog, *this);

    auto global_scope = std::make_shared<Scope>();

    // Check top-level statements if any (like in error.gene)
    for (auto& stmt : prog.top_level_stmts) {
        checkStmt(stmt.get(), global_scope, TYPE_UNKNOWN, 0);
    }

    for (auto& gene : prog.genes) {
        if (gene->name == "nucleus") {
            has_nucleus = true;
            if (!gene->params.empty()) {
                reportError(gene->loc, "gene nucleus() must take 0 parameters.",
                            "Execution starts at nucleus(); it takes no arguments.");
            }
        }

        auto gene_scope = std::make_shared<Scope>(global_scope);
        for (auto& p : gene->params) {
            Symbol sym;
            sym.name = p.name;
            sym.type = p.type;
            sym.is_counter = false;
            sym.loc = p.loc;
            if (!gene_scope->declare(sym)) {
                reportError(p.loc, "duplicate parameter name '" + p.name + "' in gene '" + gene->name + "'.");
            }
        }

        DataType ret_type = gene_signatures[gene->name].return_type;
        for (auto& s : gene->body) {
            checkStmt(s.get(), gene_scope, ret_type, 0);
        }
    }

    if (!has_nucleus && prog.top_level_stmts.empty()) {
        reportError({1, 1}, "missing entry point 'gene nucleus()'.",
                    "GeneScript programs must define a gene nucleus() with no parameters.");
    }

    return errors.empty();
}
