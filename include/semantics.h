#ifndef GENESCRIPT_SEMANTICS_H
#define GENESCRIPT_SEMANTICS_H

#include "ast.h"
#include "symbol_table.h"
#include <string>
#include <vector>
#include <unordered_map>

struct GeneSignature {
    std::string name;
    std::vector<DataType> param_types;
    DataType return_type;
    SourceLoc loc;
};

class SemanticAnalyzer {
public:
    std::string filename;
    std::vector<std::string> errors;
    std::unordered_map<std::string, GeneSignature> gene_signatures;
    bool has_nucleus = false;

    SemanticAnalyzer(const std::string& filename) : filename(filename) {}

    bool analyze(ProgramAST& prog);

    void reportError(SourceLoc loc, const std::string& msg, const std::string& note = "");

    DataType validateLiteral(LiteralExprAST* lit, DataType expected_context_type);
    DataType checkExpr(ExprAST* expr, std::shared_ptr<Scope> scope, DataType expected = TYPE_UNKNOWN);
    void checkStmt(StmtAST* stmt, std::shared_ptr<Scope> scope, DataType enclosing_gene_ret, int loop_depth);
    void checkGene(GeneDeclAST* gene);

    bool isValidDNA(const std::string& s, char& invalid_char);
    bool isValidRNA(const std::string& s, char& invalid_char);
};

#endif // GENESCRIPT_SEMANTICS_H
