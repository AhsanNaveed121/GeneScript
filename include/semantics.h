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

    // Return type inference (silent, no errors reported)
    void inferReturnTypes(ProgramAST& prog);
    DataType inferExprType(ExprAST* expr, std::shared_ptr<Scope> scope);
    void collectExpressTypes(const std::vector<std::unique_ptr<StmtAST>>& body,
                             std::shared_ptr<Scope> scope, std::vector<DataType>& out);

    // Proposal rules for integers and single bases
    DataType checkLenOperand(UnaryExprAST* len, std::shared_ptr<Scope> scope);
    DataType checkIntPosition(ExprAST* expr, std::shared_ptr<Scope> scope, DataType expected = TYPE_UNKNOWN);
    void checkCycleBound(ExprAST* bound, std::shared_ptr<Scope> scope, const std::string& which);
    bool isNotSingleBase(ExprAST* expr, std::shared_ptr<Scope> scope);

    bool isValidDNA(const std::string& s, char& invalid_char);
    bool isValidRNA(const std::string& s, char& invalid_char);
};

#endif // GENESCRIPT_SEMANTICS_H
