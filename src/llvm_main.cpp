// Development driver for the Phase 2 LLVM backend (Part 1).
//
//   bin/genescript-llvm <source.gene> [-o <out.ll>]
//
// Runs the same front end as bin/genescript (lexer, parser, semantic
// analysis, identical diagnostics), then emits LLVM IR instead of
// interpreting.  Without -o the IR goes to stdout.  The final `genec` CLI
// that also invokes clang is Part 4; until then link manually:
//
//   clang out.ll runtime/gs_runtime.c -o program

#include "ast.h"
#include "codegen.h"
#include "semantics.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

extern FILE* yyin;
extern int yyparse();
extern ProgramAST* g_program;
extern const char* g_source_name;
extern int g_lex_errors;

static void printStopped(size_t count) {
    std::cerr << "Compilation stopped: " << count << (count == 1 ? " error.\n" : " errors.\n");
}

int main(int argc, char** argv) {
    std::string filename, out_path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (filename.empty()) {
            filename = a;
        } else {
            filename.clear();
            break;
        }
    }
    if (filename.empty()) {
        std::cerr << "Usage: " << argv[0] << " <source.gene> [-o <out.ll>]\n";
        return 1;
    }

    FILE* f = fopen(filename.c_str(), "r");
    if (!f) {
        std::cerr << "Error: Cannot open file '" << filename << "'\n";
        return 1;
    }
    yyin = f;
    g_source_name = filename.c_str();
    int parse_result = yyparse();
    fclose(f);

    if (g_lex_errors > 0 || parse_result != 0) {
        printStopped(g_lex_errors + (parse_result != 0 ? 1 : 0));
        delete g_program;
        return 1;
    }
    if (!g_program) g_program = new ProgramAST();

    SemanticAnalyzer sem(filename);
    if (!sem.analyze(*g_program)) {
        for (const auto& err : sem.errors) std::cerr << err;
        printStopped(sem.errors.size());
        delete g_program;
        return 1;
    }

    CodeGenerator gen(filename, sem);
    std::ostringstream ir;
    if (!gen.generate(*g_program, ir)) {
        for (const auto& err : gen.errors) std::cerr << err;
        printStopped(gen.errors.size());
        delete g_program;
        return 1;
    }
    delete g_program;

    if (out_path.empty()) {
        std::cout << ir.str();
    } else {
        std::ofstream out(out_path);
        if (!out || !(out << ir.str())) {
            std::cerr << "Error: Cannot write '" << out_path << "'\n";
            return 1;
        }
    }
    return 0;
}
