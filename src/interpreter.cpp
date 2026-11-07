#include "interpreter.h"
#include <algorithm>
#include <iostream>

Value LiteralExprAST::evaluate(InterpreterContext& ctx) {
    (void)ctx;
    Value v;
    v.type = deduced_type != TYPE_UNKNOWN ? deduced_type : TYPE_DNA;
    v.str_val = value;
    return v;
}

Value IntLiteralExprAST::evaluate(InterpreterContext& ctx) {
    (void)ctx;
    return Value::makeInt(value);
}

Value BoolLiteralExprAST::evaluate(InterpreterContext& ctx) {
    (void)ctx;
    return Value::makeBool(value);
}

Value VarExprAST::evaluate(InterpreterContext& ctx) {
    return ctx.getVar(name);
}

Value BinaryExprAST::evaluate(InterpreterContext& ctx) {
    if (op == OP_BOTH) {
        Value l = lhs->evaluate(ctx);
        if (!l.bool_val) return Value::makeBool(false);
        Value r = rhs->evaluate(ctx);
        return Value::makeBool(r.bool_val);
    }
    if (op == OP_EITHER) {
        Value l = lhs->evaluate(ctx);
        if (l.bool_val) return Value::makeBool(true);
        Value r = rhs->evaluate(ctx);
        return Value::makeBool(r.bool_val);
    }

    Value l = lhs->evaluate(ctx);
    Value r = rhs->evaluate(ctx);

    // Ordering operators compare single bases only. Literals and expressions are
    // checked at compile time; variables and gene results are checked here.
    if (op == OP_LT || op == OP_GT || op == OP_LE || op == OP_GE) {
        if (l.str_val.length() != 1 || r.str_val.length() != 1) {
            throw std::runtime_error("Runtime Error (line " + std::to_string(loc.line) +
                                     "): ordering operators compare single bases only, but got \"" +
                                     l.str_val + "\" and \"" + r.str_val + "\".");
        }
    }

    switch (op) {
        case OP_FUSE: {
            Value res;
            res.type = l.type != TYPE_UNKNOWN ? l.type : r.type;
            res.str_val = l.str_val + r.str_val;
            return res;
        }
        case OP_EQ:
            return Value::makeBool(l.str_val == r.str_val);
        case OP_LT:
            return Value::makeBool(l.str_val < r.str_val);
        case OP_GT:
            return Value::makeBool(l.str_val > r.str_val);
        case OP_LE:
            return Value::makeBool(l.str_val <= r.str_val);
        case OP_GE:
            return Value::makeBool(l.str_val >= r.str_val);
        default:
            return Value::makeBool(false);
    }
}

Value UnaryExprAST::evaluate(InterpreterContext& ctx) {
    Value val = operand->evaluate(ctx);
    switch (op) {
        case OP_LACKS:
            return Value::makeBool(!val.bool_val);
        case OP_LEN:
            return Value::makeInt(val.str_val.length());
        case OP_REVCOMP: {
            // Reverse complement for DNA:
            // Swap base: A<->T, C<->G, then reverse
            std::string res;
            res.reserve(val.str_val.length());
            for (auto it = val.str_val.rbegin(); it != val.str_val.rend(); ++it) {
                char c = *it;
                if (c == 'A') res.push_back('T');
                else if (c == 'T') res.push_back('A');
                else if (c == 'C') res.push_back('G');
                else if (c == 'G') res.push_back('C');
                else res.push_back(c);
            }
            Value v;
            v.type = TYPE_DNA;
            v.str_val = res;
            return v;
        }
    }
    return Value();
}

Value BuiltinCallExprAST::evaluate(InterpreterContext& ctx) {
    Value val = arg->evaluate(ctx);
    switch (op) {
        case BUILTIN_HEAD: {
            Value res;
            res.type = val.type;
            if (val.str_val.empty()) {
                res.str_val = "";
            } else {
                res.str_val = val.str_val.substr(0, 1);
            }
            return res;
        }
        case BUILTIN_TAIL: {
            Value res;
            res.type = val.type;
            if (val.str_val.length() <= 1) {
                res.str_val = "";
            } else {
                res.str_val = val.str_val.substr(1);
            }
            return res;
        }
        case BUILTIN_TRANSCRIBE: {
            Value res;
            res.type = TYPE_RNA;
            std::string s = val.str_val;
            for (char& c : s) {
                if (c == 'T') c = 'U';
            }
            res.str_val = s;
            return res;
        }
    }
    return Value();
}

Value IndexExprAST::evaluate(InterpreterContext& ctx) {
    Value s = seq->evaluate(ctx);
    Value idx = index->evaluate(ctx);
    long long i = idx.int_val;
    Value res;
    res.type = s.type;
    if (i >= 0 && (size_t)i < s.str_val.length()) {
        res.str_val = s.str_val.substr(i, 1);
    } else {
        res.str_val = "";
    }
    return res;
}

Value CallExprAST::evaluate(InterpreterContext& ctx) {
    std::vector<Value> arg_vals;
    for (auto& a : args) {
        arg_vals.push_back(a->evaluate(ctx));
    }
    return ctx.callGene(callee, arg_vals);
}

ExecStatus DeclStmtAST::execute(InterpreterContext& ctx, Value& return_val) {
    (void)return_val;
    Value v = init_expr->evaluate(ctx);
    v.type = declared_type;
    ctx.setVar(var_name, v);
    return STATUS_NORMAL;
}

ExecStatus AssignStmtAST::execute(InterpreterContext& ctx, Value& return_val) {
    (void)return_val;
    Value v = expr->evaluate(ctx);
    ctx.assignVar(var_name, v);
    return STATUS_NORMAL;
}

ExecStatus ReportStmtAST::execute(InterpreterContext& ctx, Value& return_val) {
    (void)return_val;
    Value v = expr->evaluate(ctx);
    std::cout << v.str_val << std::endl;
    return STATUS_NORMAL;
}

ExecStatus ExpressStmtAST::execute(InterpreterContext& ctx, Value& return_val) {
    return_val = expr->evaluate(ctx);
    return STATUS_RETURN;
}

ExecStatus WhenStmtAST::execute(InterpreterContext& ctx, Value& return_val) {
    for (auto& b : branches) {
        Value c = b.condition->evaluate(ctx);
        if (c.bool_val) {
            for (auto& s : b.body) {
                ExecStatus st = s->execute(ctx, return_val);
                if (st != STATUS_NORMAL) return st;
            }
            return STATUS_NORMAL;
        }
    }
    for (auto& s : otherwise_body) {
        ExecStatus st = s->execute(ctx, return_val);
        if (st != STATUS_NORMAL) return st;
    }
    return STATUS_NORMAL;
}

ExecStatus CycleAcrossStmtAST::execute(InterpreterContext& ctx, Value& return_val) {
    Value s = seq_expr->evaluate(ctx);//gets the DNA Sequence
    long long len = s.str_val.length();// calcultes its length
    for (long long i = 0; i < len; ++i) { // visits every index
        ctx.setVar(counter_var, Value::makeInt(i));//sets the GeneScript loop counter i
        for (auto& stmt : body) {
            ExecStatus st = stmt->execute(ctx, return_val);//executes each statment inside the genescript loop
            if (st == STATUS_HALT) return STATUS_NORMAL;
            if (st == STATUS_SKIP) break;
            if (st == STATUS_RETURN) return STATUS_RETURN;
        }
    }
    return STATUS_NORMAL;
}

ExecStatus CycleFromToStmtAST::execute(InterpreterContext& ctx, Value& return_val) {
    Value vfrom = from_expr->evaluate(ctx);
    Value vto = to_expr->evaluate(ctx);
    long long from = vfrom.int_val;
    long long to = vto.int_val;
    for (long long i = from; i < to; ++i) {
        ctx.setVar(counter_var, Value::makeInt(i));
        for (auto& stmt : body) {
            ExecStatus st = stmt->execute(ctx, return_val);
            if (st == STATUS_HALT) return STATUS_NORMAL;
            if (st == STATUS_SKIP) break;
            if (st == STATUS_RETURN) return STATUS_RETURN;
        }
    }
    return STATUS_NORMAL;
}

ExecStatus GrowUntilStmtAST::execute(InterpreterContext& ctx, Value& return_val) {
    while (true) {
        Value cond = cond_expr->evaluate(ctx);
        if (cond.bool_val) {
            break; // loop until condition is met
        }
        bool should_break = false;
        for (auto& stmt : body) {
            ExecStatus st = stmt->execute(ctx, return_val);
            if (st == STATUS_HALT) { should_break = true; break; }
            if (st == STATUS_SKIP) break;
            if (st == STATUS_RETURN) return STATUS_RETURN;
        }
        if (should_break) break;
    }
    return STATUS_NORMAL;
}

ExecStatus HaltStmtAST::execute(InterpreterContext& ctx, Value& return_val) {
    (void)ctx;
    (void)return_val;
    return STATUS_HALT;
}

ExecStatus SkipStmtAST::execute(InterpreterContext& ctx, Value& return_val) {
    (void)ctx;
    (void)return_val;
    return STATUS_SKIP;
}

Value InterpreterContext::callGene(const std::string& name, const std::vector<Value>& arg_values) {
    auto it = genes.find(name);
    if (it == genes.end()) {
        throw std::runtime_error("Runtime Error: Undefined gene '" + name + "'");
    }

    GeneDeclAST* gene = it->second;
    pushFrame();

    for (size_t i = 0; i < gene->params.size(); ++i) {
        setVar(gene->params[i].name, arg_values[i]);
    }

    Value return_val;
    for (auto& stmt : gene->body) {
        ExecStatus st = stmt->execute(*this, return_val);
        if (st == STATUS_RETURN) {
            break;
        }
    }

    popFrame();
    return return_val;
}

void InterpreterContext::executeProgram(ProgramAST& program) {
    // Register all genes
    for (auto& gene : program.genes) {
        genes[gene->name] = gene.get();
    }

    // Execute top level statements if any
    Value dummy;
    for (auto& stmt : program.top_level_stmts) {
        stmt->execute(*this, dummy);
    }

    // Run nucleus if defined
    if (genes.find("nucleus") != genes.end()) {
        callGene("nucleus", {});
    }
}
