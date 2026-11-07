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
            if (!requireSequence(t1, bin->loc, "fuse") || !requireSequence(t2, bin->loc, "fuse")) {
                return TYPE_UNKNOWN;
            }
            if (t1 != TYPE_UNKNOWN && t2 != TYPE_UNKNOWN && t1 != t2) {
                reportError(bin->loc, "cannot fuse " + dataTypeToString(t1) + " with " + dataTypeToString(t2) + ".",
                            "fuse requires both sequences to have the same biological type.");
                return TYPE_UNKNOWN;
            }
            bin->deduced_type = (t1 != TYPE_UNKNOWN) ? t1 : t2;
            return bin->deduced_type;
        }
        // Type both sides of a comparison. A literal without T or U (like 'G' or "")
        // has no type of its own, so it takes its type from the other side.
        auto checkComparisonOperands = [&](DataType& t1, DataType& t2) {
            auto* llit = dynamic_cast<LiteralExprAST*>(bin->lhs.get());
            bool lhs_ambiguous = llit && llit->value.find('T') == std::string::npos &&
                                 llit->value.find('U') == std::string::npos;
            if (lhs_ambiguous) {
                t2 = checkExpr(bin->rhs.get(), scope, TYPE_UNKNOWN);
                t1 = checkExpr(bin->lhs.get(), scope, t2);
            } else {
                t1 = checkExpr(bin->lhs.get(), scope, TYPE_UNKNOWN);
                t2 = checkExpr(bin->rhs.get(), scope, t1);
                if (t1 == TYPE_UNKNOWN && t2 != TYPE_UNKNOWN) {
                    t1 = checkExpr(bin->lhs.get(), scope, t2);
                }
            }
        };
        if (bin->op == OP_EQ) {
            DataType t1, t2;
            checkComparisonOperands(t1, t2);
            if (t1 != TYPE_UNKNOWN && t2 != TYPE_UNKNOWN && t1 != t2) {
                reportError(bin->loc, "cannot compare " + dataTypeToString(t1) + " with " + dataTypeToString(t2) + " using '='.",
                            "comparison '=' requires sequences of the same type.");
            } else {
                requireSequence(t1, bin->loc, "comparison '='");
            }
            bin->deduced_type = TYPE_BOOL;
            return TYPE_BOOL;
        }
        if (bin->op == OP_LT || bin->op == OP_GT || bin->op == OP_LE || bin->op == OP_GE) {
            static const char* op_names[] = {"", "", "<", ">", "<=", ">="};
            std::string op_name = op_names[bin->op];
            DataType t1, t2;
            checkComparisonOperands(t1, t2);
            if (isNotSingleBase(bin->lhs.get(), scope) || isNotSingleBase(bin->rhs.get(), scope)) {
                reportError(bin->loc, "ordering operator '" + op_name + "' compares single bases only.",
                            "use a base such as 'G', head(s) or s@i; compare whole sequences with '='.");
            } else if (t1 != TYPE_UNKNOWN && t2 != TYPE_UNKNOWN && t1 != t2) {
                reportError(bin->loc, "cannot compare " + dataTypeToString(t1) + " with " + dataTypeToString(t2) + ".",
                            "ordering comparisons (<, >, <=, >=) compare bases of the same type.");
            }
            bin->deduced_type = TYPE_BOOL;
            return TYPE_BOOL;
        }
        if (bin->op == OP_BOTH || bin->op == OP_EITHER) {
            const char* word = bin->op == OP_BOTH ? "'both'" : "'either'";
            checkCondition(bin->lhs.get(), scope, word);
            checkCondition(bin->rhs.get(), scope, word);
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
            checkCondition(un->operand.get(), scope, "'lacks'");
            un->deduced_type = TYPE_BOOL;
            return TYPE_BOOL;
        }
        if (un->op == OP_LEN) {
            // Valid uses of #s (cycle bounds) are handled by checkCycleBound and never
            // reach this point, so any # seen here is misplaced.
            checkLenOperand(un, scope);
            reportError(un->loc, "length '#' can only be used as a loop bound.",
                        "#s cannot be stored, compared, passed, returned or reported; use it as in 'cycle i from 0 to #s'.");
            return TYPE_UNKNOWN;
        }
    }
    if (auto* bc = dynamic_cast<BuiltinCallExprAST*>(expr)) {
        if (bc->op == BUILTIN_HEAD || bc->op == BUILTIN_TAIL) {
            DataType t = checkExpr(bc->arg.get(), scope, expected);
            if (!requireSequence(t, bc->loc, bc->op == BUILTIN_HEAD ? "head()" : "tail()")) {
                return TYPE_UNKNOWN;
            }
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
        if (!requireSequence(tseq, idx->loc, "indexing (@)")) tseq = TYPE_UNKNOWN;
        bool valid_index = false;
        if (dynamic_cast<IntLiteralExprAST*>(idx->index.get())) {
            valid_index = true;
        } else if (auto* iv = dynamic_cast<VarExprAST*>(idx->index.get())) {
            Symbol* sym = scope->lookup(iv->name);
            valid_index = !sym || sym->is_counter; // undeclared is reported by checkExpr
        }
        checkIntPosition(idx->index.get(), scope);
        if (!valid_index) {
            reportError(idx->loc, "position in s@i must be a loop counter or an integer literal.",
                        "write s@i inside 'cycle i ...' or use a fixed position such as s@0.");
        }
        idx->deduced_type = tseq;
        return tseq;
    }
    if (auto* call = dynamic_cast<CallExprAST*>(expr)) {
        auto it = gene_signatures.find(call->callee);
        if (it == gene_signatures.end()) {
            reportError(call->loc, "call to undefined gene '" + call->callee + "'.",
                        "define it with 'gene " + call->callee + "(...) ... stop' anywhere in the file.");
            return TYPE_UNKNOWN;
        }
        const auto& sig = it->second;
        if (sig.param_types.size() != call->args.size()) {
            reportError(call->loc, "gene '" + call->callee + "' expects " +
                        std::to_string(sig.param_types.size()) + " arguments, but got " +
                        std::to_string(call->args.size()) + ".",
                        "pass one DNA or RNA sequence for each parameter of the gene.");
        } else {
            for (size_t i = 0; i < call->args.size(); ++i) {
                DataType param_t = sig.param_types[i];
                DataType arg_t = checkIntPosition(call->args[i].get(), scope, param_t);
                if (arg_t == TYPE_UNKNOWN || arg_t == param_t) continue;
                std::string note;
                if (arg_t == TYPE_INT) {
                    note = "integers (such as #s or loop counters) cannot be passed to a gene; only DNA or RNA can.";
                } else if (arg_t == TYPE_BOOL) {
                    note = "bound/unbound values cannot be passed to a gene; only DNA or RNA can.";
                } else {
                    note = "DNA and RNA are separate types; a parameter accepts only its declared type.";
                }
                reportError(call->args[i]->loc, "argument " + std::to_string(i + 1) + " of gene '" + call->callee +
                            "' must be " + dataTypeToString(param_t) + ", but got " + dataTypeToString(arg_t) + ".",
                            note);
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
        if (t == TYPE_INT || t == TYPE_BOOL) {
            reportError(exp->loc, "a gene can only express DNA or RNA, not " + dataTypeToString(t) + ".",
                        "every value returned by a gene must be a biological sequence.");
        } else if (enclosing_gene_ret != TYPE_UNKNOWN && t != TYPE_UNKNOWN && t != enclosing_gene_ret) {
            reportError(exp->loc, "gene return type mismatch: returning " + dataTypeToString(t) +
                        " but expected " + dataTypeToString(enclosing_gene_ret) + ".",
                        "every express statement in a gene must return the same sequence type.");
        }
        return;
    }

    if (auto* w = dynamic_cast<WhenStmtAST*>(stmt)) {
        for (auto& b : w->branches) {
            checkCondition(b.condition.get(), scope, "'when'");
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
        DataType across_t = checkIntPosition(cyc->seq_expr.get(), scope);
        requireSequence(across_t, cyc->loc, "'cycle ... across'");
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
        checkCycleBound(cft->from_expr.get(), scope, "from");
        checkCycleBound(cft->to_expr.get(), scope, "to");
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
        checkCondition(gr->cond_expr.get(), scope, "'grow until'");
        auto loop_scope = std::make_shared<Scope>(scope);
        for (auto& s : gr->body) {
            checkStmt(s.get(), loop_scope, enclosing_gene_ret, loop_depth + 1);
        }
        return;
    }

    if (dynamic_cast<HaltStmtAST*>(stmt)) {
        if (loop_depth == 0) {
            reportError(stmt->loc, "halt (break) statement outside of any loop.",
                        "halt leaves the innermost cycle or grow until loop; a when block is not a loop.");
        }
        return;
    }

    if (dynamic_cast<SkipStmtAST*>(stmt)) {
        if (loop_depth == 0) {
            reportError(stmt->loc, "skip (continue) statement outside of any loop.",
                        "skip moves to the next pass of the innermost cycle or grow until loop.");
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Conditions and sequence operands. Integers (loop counters) and bound/unbound
// are not sequences, and sequences are not conditions.
// ---------------------------------------------------------------------------

// Reports an error when `t` is an integer or boolean where a DNA/RNA sequence
// is required. Returns false in that case (unknown types pass silently).
bool SemanticAnalyzer::requireSequence(DataType t, SourceLoc loc, const std::string& what) {
    if (t != TYPE_INT && t != TYPE_BOOL) return true;
    reportError(loc, what + " needs a DNA or RNA sequence, not " + dataTypeToString(t) + ".",
                "loop counters and bound/unbound values are not sequences.");
    return false;
}

// Conditions of when / grow until / both / either / lacks must be boolean:
// bound, unbound, a comparison, or a combination of those.
void SemanticAnalyzer::checkCondition(ExprAST* expr, std::shared_ptr<Scope> scope, const std::string& where) {
    DataType t = checkIntPosition(expr, scope, TYPE_BOOL);
    if (t != TYPE_BOOL && t != TYPE_UNKNOWN) {
        reportError(expr->loc, where + " needs a condition, not " + dataTypeToString(t) + ".",
                    "use bound, unbound or a comparison such as 's = \"\"' or 's@i < 'G''.");
    }
}

// ---------------------------------------------------------------------------
// Integer rules from the proposal: #s only as a loop bound, s@i positions,
// and cycle bounds.
// ---------------------------------------------------------------------------

// Checks the operand of #s without reporting the "# outside a loop bound" error.
DataType SemanticAnalyzer::checkLenOperand(UnaryExprAST* len, std::shared_ptr<Scope> scope) {
    DataType t = checkExpr(len->operand.get(), scope, TYPE_UNKNOWN);
    if (t == TYPE_INT || t == TYPE_BOOL) {
        reportError(len->loc, "length '#' requires a DNA or RNA sequence, not " + dataTypeToString(t) + ".",
                    "#s counts the bases in the sequence s.");
    }
    len->deduced_type = TYPE_INT;
    return TYPE_INT;
}

// Checks an expression in a position where the caller reports a more specific
// error than "misplaced #" (gene arguments, s@i positions, cycle bounds).
DataType SemanticAnalyzer::checkIntPosition(ExprAST* expr, std::shared_ptr<Scope> scope, DataType expected) {
    auto* un = dynamic_cast<UnaryExprAST*>(expr);
    if (un && un->op == OP_LEN) {
        return checkLenOperand(un, scope);
    }
    return checkExpr(expr, scope, expected);
}

// cycle i from a to b: a and b may only be integer literals, #s, or an enclosing loop counter.
void SemanticAnalyzer::checkCycleBound(ExprAST* bound, std::shared_ptr<Scope> scope, const std::string& which) {
    bool valid = false;
    if (dynamic_cast<IntLiteralExprAST*>(bound)) {
        valid = true;
    } else if (auto* un = dynamic_cast<UnaryExprAST*>(bound)) {
        valid = (un->op == OP_LEN);
    } else if (auto* var = dynamic_cast<VarExprAST*>(bound)) {
        Symbol* sym = scope->lookup(var->name);
        valid = !sym || sym->is_counter; // undeclared is reported by checkExpr
    }
    checkIntPosition(bound, scope, TYPE_INT);
    if (!valid) {
        reportError(bound->loc, "cycle '" + which + "' bound must be an integer literal, #s, or an enclosing loop counter.",
                    "for example 'cycle i from 0 to #s' or 'cycle j from i to #s'.");
    }
}

// True when an operand of <, >, <=, >= can never be a single base.
// Variables and gene calls of sequence type are checked at run time instead.
bool SemanticAnalyzer::isNotSingleBase(ExprAST* expr, std::shared_ptr<Scope> scope) {
    if (auto* lit = dynamic_cast<LiteralExprAST*>(expr)) {
        return lit->value.size() != 1;
    }
    if (dynamic_cast<IntLiteralExprAST*>(expr) || dynamic_cast<BoolLiteralExprAST*>(expr)) {
        return true;
    }
    if (auto* var = dynamic_cast<VarExprAST*>(expr)) {
        Symbol* sym = scope->lookup(var->name);
        return sym && (sym->type == TYPE_INT || sym->type == TYPE_BOOL);
    }
    if (dynamic_cast<BinaryExprAST*>(expr)) {
        return true; // fuse builds sequences; other binary operators give bound/unbound
    }
    if (auto* un = dynamic_cast<UnaryExprAST*>(expr)) {
        if (un->op == OP_REVCOMP) return isNotSingleBase(un->operand.get(), scope);
        return true;
    }
    if (auto* bc = dynamic_cast<BuiltinCallExprAST*>(expr)) {
        if (bc->op == BUILTIN_HEAD) return false;
        if (bc->op == BUILTIN_TRANSCRIBE) return isNotSingleBase(bc->arg.get(), scope);
        return true; // tail
    }
    return false; // s@i and gene calls
}

// ---------------------------------------------------------------------------
// Return type inference. Runs silently (no errors); the normal checking pass
// afterwards reports any express statement that disagrees with the result.
// ---------------------------------------------------------------------------

DataType SemanticAnalyzer::inferExprType(ExprAST* expr, std::shared_ptr<Scope> scope) {
    if (!expr) return TYPE_UNKNOWN;
    if (auto* lit = dynamic_cast<LiteralExprAST*>(expr)) {
        // Only T or U fixes a literal's type; "" or "ACG" depends on context.
        if (lit->value.find('T') != std::string::npos) return TYPE_DNA;
        if (lit->value.find('U') != std::string::npos) return TYPE_RNA;
        return TYPE_UNKNOWN;
    }
    if (dynamic_cast<IntLiteralExprAST*>(expr)) return TYPE_INT;
    if (dynamic_cast<BoolLiteralExprAST*>(expr)) return TYPE_BOOL;
    if (auto* var = dynamic_cast<VarExprAST*>(expr)) {
        Symbol* sym = scope->lookup(var->name);
        return sym ? sym->type : TYPE_UNKNOWN;
    }
    if (auto* bin = dynamic_cast<BinaryExprAST*>(expr)) {
        if (bin->op != OP_FUSE) return TYPE_BOOL;
        DataType t = inferExprType(bin->lhs.get(), scope);
        return t != TYPE_UNKNOWN ? t : inferExprType(bin->rhs.get(), scope);
    }
    if (auto* un = dynamic_cast<UnaryExprAST*>(expr)) {
        if (un->op == OP_REVCOMP) return TYPE_DNA;
        if (un->op == OP_LACKS) return TYPE_BOOL;
        return TYPE_INT;
    }
    if (auto* bc = dynamic_cast<BuiltinCallExprAST*>(expr)) {
        if (bc->op == BUILTIN_TRANSCRIBE) return TYPE_RNA;
        return inferExprType(bc->arg.get(), scope);
    }
    if (auto* idx = dynamic_cast<IndexExprAST*>(expr)) {
        return inferExprType(idx->seq.get(), scope);
    }
    if (auto* call = dynamic_cast<CallExprAST*>(expr)) {
        auto it = gene_signatures.find(call->callee);
        return it != gene_signatures.end() ? it->second.return_type : TYPE_UNKNOWN;
    }
    return TYPE_UNKNOWN;
}

// Collects the type of every express statement in a body, including those nested
// in when / cycle / grow blocks, while tracking declared variables and counters.
void SemanticAnalyzer::collectExpressTypes(const std::vector<std::unique_ptr<StmtAST>>& body,
                                           std::shared_ptr<Scope> scope, std::vector<DataType>& out) {
    for (const auto& stmt : body) {
        StmtAST* s = stmt.get();
        if (auto* decl = dynamic_cast<DeclStmtAST*>(s)) {
            scope->declare({decl->var_name, decl->declared_type, false, decl->loc});
        } else if (auto* exp = dynamic_cast<ExpressStmtAST*>(s)) {
            out.push_back(inferExprType(exp->expr.get(), scope));
        } else if (auto* w = dynamic_cast<WhenStmtAST*>(s)) {
            for (const auto& b : w->branches) {
                collectExpressTypes(b.body, std::make_shared<Scope>(scope), out);
            }
            collectExpressTypes(w->otherwise_body, std::make_shared<Scope>(scope), out);
        } else if (auto* ca = dynamic_cast<CycleAcrossStmtAST*>(s)) {
            auto loop_scope = std::make_shared<Scope>(scope);
            loop_scope->declare({ca->counter_var, TYPE_INT, true, ca->loc});
            collectExpressTypes(ca->body, loop_scope, out);
        } else if (auto* cft = dynamic_cast<CycleFromToStmtAST*>(s)) {
            auto loop_scope = std::make_shared<Scope>(scope);
            loop_scope->declare({cft->counter_var, TYPE_INT, true, cft->loc});
            collectExpressTypes(cft->body, loop_scope, out);
        } else if (auto* gr = dynamic_cast<GrowUntilStmtAST*>(s)) {
            collectExpressTypes(gr->body, std::make_shared<Scope>(scope), out);
        }
    }
}

static bool sameLoc(SourceLoc a, SourceLoc b) {
    return a.line == b.line && a.col == b.col;
}

// Repeats until no gene's return type changes, so a gene that expresses the
// result of another gene (in any order, including mutual recursion) is resolved.
void SemanticAnalyzer::inferReturnTypes(ProgramAST& prog) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& gene : prog.genes) {
            GeneSignature& sig = gene_signatures[gene->name];
            if (!sameLoc(sig.loc, gene->loc)) continue; // duplicate definition
            if (sig.return_type != TYPE_UNKNOWN) continue;

            auto scope = std::make_shared<Scope>();
            for (const auto& p : gene->params) {
                scope->declare({p.name, p.type, false, p.loc});
            }
            std::vector<DataType> types;
            collectExpressTypes(gene->body, scope, types);
            for (DataType t : types) {
                if (t == TYPE_DNA || t == TYPE_RNA) {
                    sig.return_type = t;
                    changed = true;
                    break;
                }
            }
        }
    }
    // Nothing fixes the type (no express, or only literals like ""): default to DNA,
    // matching the proposal's rule for ambiguous literals.
    for (auto& entry : gene_signatures) {
        if (entry.second.return_type == TYPE_UNKNOWN) {
            entry.second.return_type = TYPE_DNA;
        }
    }
}

bool SemanticAnalyzer::analyze(ProgramAST& prog) {
    // Pass 1: collect gene signatures (allows forward calls & mutual recursion)
    // and reject duplicate gene names.
    for (auto& gene : prog.genes) {
        auto existing = gene_signatures.find(gene->name);
        if (existing != gene_signatures.end()) {
            std::string first_line = std::to_string(existing->second.loc.line);
            if (gene->name == "nucleus") {
                reportError(gene->loc, "multiple definitions of entry point 'gene nucleus()'.",
                            "a program must have exactly one nucleus(); the first one is at line " + first_line + ".");
            } else {
                reportError(gene->loc, "duplicate gene '" + gene->name + "'.",
                            "gene '" + gene->name + "' is already defined at line " + first_line +
                            "; each gene name must be unique.");
            }
            continue;
        }
        GeneSignature sig;
        sig.name = gene->name;
        sig.loc = gene->loc;
        for (const auto& p : gene->params) {
            sig.param_types.push_back(p.type);
        }
        sig.return_type = TYPE_UNKNOWN;
        gene_signatures[gene->name] = sig;
    }

    // Pass 2: infer return types from all express statements.
    inferReturnTypes(prog);

    auto global_scope = std::make_shared<Scope>();

    // Check top-level statements if any (like in error.gene)
    size_t errors_before_top_level = errors.size();
    for (auto& stmt : prog.top_level_stmts) {
        checkStmt(stmt.get(), global_scope, TYPE_UNKNOWN, 0);
    }
    bool top_level_had_errors = errors.size() > errors_before_top_level;
    // Execution starts at nucleus(), so statements outside every gene would
    // never belong to the program. (A file of only top-level statements, like
    // the proposal's error.gene snippet, gets the missing nucleus() error below.)
    if (!prog.genes.empty()) {
        for (auto& stmt : prog.top_level_stmts) {
            reportError(stmt->loc, "statement outside any gene.",
                        "every statement must be inside a gene; the program starts at gene nucleus().");
        }
    }

    // Pass 3: check every gene body.
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

        // A duplicate definition has no signature of its own; check its body without a return type.
        const GeneSignature& sig = gene_signatures[gene->name];
        DataType ret_type = sameLoc(sig.loc, gene->loc) ? sig.return_type : TYPE_UNKNOWN;
        for (auto& s : gene->body) {
            checkStmt(s.get(), gene_scope, ret_type, 0);
        }
    }

    // Every program needs exactly one nucleus(). For a file of only top-level
    // statements that already has errors (the proposal's error.gene snippet),
    // those errors are reported alone, matching the proposal's sample output.
    if (!has_nucleus && !(prog.genes.empty() && top_level_had_errors)) {
        reportError({1, 1}, "missing entry point 'gene nucleus()'.",
                    "GeneScript programs must define a gene nucleus() with no parameters.");
    }

    return errors.empty();
}
