// genec: the GeneScript command-line driver (Phase 1 interpreter + Phase 2
// ahead-of-time compiler).
//
//   genec file.gene                  run with the Phase 1 interpreter
//   genec -S file.gene [-o out.ll]   write LLVM IR (default: <name>.ll)
//   genec -c file.gene [-o prog]     compile to a native executable
//                                    (default: <name>)
//
// All modes share the same front end (lexer, parser, semantic analysis), so
// they report identical diagnostics, and nothing is written when the program
// has errors.  -c writes the IR to a temporary file and runs
//
//   clang -O2 <tmp>.ll <runtime>/gs_runtime.c -o <prog>
//
// Environment: CLANG     compiler to run (default "clang")
//              GS_RUNTIME path to gs_runtime.c (default: found next to genec)

#include "ast.h"
#include "codegen.h"
#include "interpreter.h"
#include "semantics.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern FILE* yyin;
extern int yyparse();
extern ProgramAST* g_program;
extern const char* g_source_name;
extern int g_lex_errors;

namespace {

enum class Mode { Interpret, EmitIR, Compile };

void printStopped(size_t count) {
    std::cerr << "Compilation stopped: " << count << (count == 1 ? " error.\n" : " errors.\n");
}

void usage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " <source.gene>                 run with the interpreter\n"
              << "       " << argv0 << " -S <source.gene> [-o out.ll]  write LLVM IR\n"
              << "       " << argv0 << " -c <source.gene> [-o prog]    compile to a native executable\n";
}

bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// "dir/merge_sort.gene" -> "merge_sort"
std::string stem(const std::string& path) {
    std::string base = path.substr(path.find_last_of('/') + 1);
    size_t dot = base.rfind('.');
    if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);
    return base;
}

// Lexer, parser and semantic analysis.  Prints diagnostics and returns false
// if the program has any error.
bool runFrontEnd(const std::string& filename, SemanticAnalyzer& sem) {
    FILE* f = fopen(filename.c_str(), "r");
    if (!f) {
        std::cerr << "Error: Cannot open file '" << filename << "'\n";
        return false;
    }
    yyin = f;
    g_source_name = filename.c_str();
    int parse_result = yyparse();
    fclose(f);

    if (g_lex_errors > 0 || parse_result != 0) {
        printStopped(g_lex_errors + (parse_result != 0 ? 1 : 0));
        return false;
    }
    if (!g_program) g_program = new ProgramAST();  // empty file: missing nucleus()

    if (!sem.analyze(*g_program)) {
        for (const auto& err : sem.errors) std::cerr << err;
        printStopped(sem.errors.size());
        return false;
    }
    return true;
}

// gs_runtime.c: $GS_RUNTIME, else runtime/ next to the genec executable or
// one directory above it (for bin/genec).
std::string findRuntime() {
    if (const char* env = std::getenv("GS_RUNTIME")) {
        return fileExists(env) ? env : "";
    }
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    std::string dir(buf);
    dir = dir.substr(0, dir.find_last_of('/'));
    for (const std::string& candidate : {dir + "/runtime/gs_runtime.c", dir + "/../runtime/gs_runtime.c"}) {
        if (fileExists(candidate)) return candidate;
    }
    return "";
}

// Runs a program without a shell (paths may contain spaces).  Returns its
// exit status, 127 if it could not be started, or -1 if it was killed.
int runProgram(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        execvp(argv[0], argv.data());
        _exit(127);  // could not start
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

int interpret(const std::string& filename) {
    SemanticAnalyzer sem(filename);
    if (!runFrontEnd(filename, sem)) {
        delete g_program;
        return 1;
    }
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

// Front end + code generation.  Returns false (diagnostics printed) on error.
bool generateIR(const std::string& filename, std::string& ir) {
    SemanticAnalyzer sem(filename);
    if (!runFrontEnd(filename, sem)) {
        delete g_program;
        return false;
    }
    CodeGenerator gen(filename, sem);
    std::ostringstream out;
    bool ok = gen.generate(*g_program, out);
    delete g_program;
    if (!ok) {
        for (const auto& err : gen.errors) std::cerr << err;
        printStopped(gen.errors.size());
        return false;
    }
    ir = out.str();
    return true;
}

bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out || !(out << text) || !(out.flush())) {
        std::cerr << "genec: cannot write '" << path << "': " << std::strerror(errno) << "\n";
        return false;
    }
    return true;
}

int emitIR(const std::string& filename, const std::string& out_path) {
    std::string ir;
    if (!generateIR(filename, ir)) return 1;
    return writeFile(out_path, ir) ? 0 : 1;
}

int compile(const std::string& filename, const std::string& out_path) {
    // Check the tools first so a missing runtime does not hide behind IR work.
    std::string runtime = findRuntime();
    if (runtime.empty()) {
        std::cerr << "genec: cannot find runtime/gs_runtime.c next to genec; set GS_RUNTIME to its path.\n";
        return 1;
    }
    std::string ir;
    if (!generateIR(filename, ir)) return 1;

    const char* tmpdir = std::getenv("TMPDIR");
    std::string tmpl = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") + "/genec-XXXXXX.ll";
    std::vector<char> path(tmpl.begin(), tmpl.end());
    path.push_back('\0');
    int fd = mkstemps(path.data(), 3);
    if (fd < 0) {
        std::cerr << "genec: cannot create a temporary file: " << std::strerror(errno) << "\n";
        return 1;
    }
    close(fd);
    std::string ll(path.data());
    if (!writeFile(ll, ir)) {
        unlink(ll.c_str());
        return 1;
    }

    const char* clang_env = std::getenv("CLANG");
    std::string clang = clang_env && *clang_env ? clang_env : "clang";
    int rc = runProgram({clang, "-O2", "-Wno-override-module", ll, runtime, "-o", out_path});
    unlink(ll.c_str());
    if (rc == 0) return 0;

    if (rc == 127) {
        std::cerr << "genec: could not run '" << clang
                  << "'; install clang (sudo apt install clang) or set CLANG to its path.\n";
    } else {
        std::cerr << "genec: '" << clang << "' failed (exit " << rc << "); no executable was produced.\n";
    }
    return 1;  // clang removes its own partial output on failure
}

}  // namespace

int main(int argc, char** argv) {
    Mode mode = Mode::Interpret;
    bool mode_given = false;
    std::string filename, out_path;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-S" || a == "-c") {
            Mode m = (a == "-S") ? Mode::EmitIR : Mode::Compile;
            if (mode_given && m != mode) {
                std::cerr << "genec: -S and -c cannot be used together.\n";
                return 1;
            }
            mode = m;
            mode_given = true;
        } else if (a == "-i") {
            // Phase 1 compatibility: explicit interpreter mode.
        } else if (a == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "genec: -o needs a file name.\n";
                return 1;
            }
            out_path = argv[++i];
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "genec: unknown option '" << a << "'.\n";
            usage(argv[0]);
            return 1;
        } else if (filename.empty()) {
            filename = a;
        } else {
            std::cerr << "genec: only one source file can be given ('" << filename << "' and '" << a << "').\n";
            return 1;
        }
    }
    if (filename.empty()) {
        usage(argv[0]);
        return 1;
    }

    if (mode == Mode::Interpret) {
        if (!out_path.empty()) {
            std::cerr << "genec: -o needs -S or -c.\n";
            return 1;
        }
        return interpret(filename);
    }

    if (out_path.empty()) {
        out_path = stem(filename) + (mode == Mode::EmitIR ? ".ll" : "");
    }
    if (out_path == filename) {
        std::cerr << "genec: output '" << out_path << "' would overwrite the source file; use -o.\n";
        return 1;
    }
    return mode == Mode::EmitIR ? emitIR(filename, out_path) : compile(filename, out_path);
}
