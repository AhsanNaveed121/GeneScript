#include "ast.h"
#include "semantics.h"
#include "interpreter.h"
#include <iostream>
#include <cstdio>
#include <cstring>

extern FILE* yyin;
extern int yyparse();
extern ProgramAST* g_program;
extern const char* g_source_name;
extern int g_lex_errors;

static void printStopped(size_t count) {
    std::cerr << "Compilation stopped: " << count << (count == 1 ? " error.\n" : " errors.\n");
}

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
    g_source_name = filename.c_str();
    int parse_result = yyparse();
    fclose(yyin);

    // Lexical or syntax errors: stop before semantic analysis and execution.
    if (g_lex_errors > 0 || parse_result != 0) {
        printStopped(g_lex_errors + (parse_result != 0 ? 1 : 0));
        delete g_program;
        return 1;
    }
    if (!g_program) {
        g_program = new ProgramAST(); // empty file: semantics reports the missing nucleus()
    }

    // Semantic Analysis
    SemanticAnalyzer sem(filename);
    if (!sem.analyze(*g_program)) {
        for (const auto& err : sem.errors) {
            std::cerr << err;
        }
        printStopped(sem.errors.size());
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
