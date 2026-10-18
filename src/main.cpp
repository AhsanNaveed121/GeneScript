#include "ast.h"
#include "semantics.h"
#include "interpreter.h"
#include <iostream>
#include <cstdio>
#include <cstring>

extern FILE* yyin;
extern int yyparse();
extern ProgramAST* g_program;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " [-i] <source.gene>\n";
        return 1;
    }

    std::string filename;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "-i") {
            filename = argv[i];
        }
    }

    FILE* f = fopen(filename.c_str(), "r");
    if (!f) {
        std::cerr << "Error: Cannot open file '" << filename << "'\n";
        return 1;
    }

    yyin = f;
    int parse_result = yyparse();
    fclose(yyin);

    if (parse_result != 0 || !g_program) {
        return 1;
    }

    // Semantic Analysis
    SemanticAnalyzer sem(filename);
    if (!sem.analyze(*g_program)) {
        for (const auto& err : sem.errors) {
            std::cerr << err;
        }
        std::cerr << "Compilation stopped: " << sem.errors.size()
                  << (sem.errors.size() == 1 ? " error.\n" : " errors.\n");
        delete g_program;
        return 1;
    }

    // Execution (Phase 1: Interpreter)
    try {
        InterpreterContext ctx;
        ctx.executeProgram(*g_program);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        delete g_program;
        return 1;
    }

    delete g_program;
    return 0;
}
