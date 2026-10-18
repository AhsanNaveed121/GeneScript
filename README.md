# GeneScript: A Biology-Inspired Programming Language

## Compiler Construction Project
- **Phase 1**: Interpreter Development (FLEX & Bison)
- **Phase 2**: AOT Compiler Development (LLVM IR Code Generation)
- **Course**: Compiler Construction (BCS-7G)
- **Instructor**: Sir Faisal Aslam

### Team Members
- Eman Aamir (23L-0612)
- Haleema Sadia (23L-0960)
- Ahsan Naveed (23L-0756)

---

## 1. Project Overview & Architecture

GeneScript is a domain-specific programming language designed for bioinformatics algorithms. It models biological sequence processing with built-in `dna` and `rna` types, value-first assignments, prefix concatenation (`fuse a b`), reverse complement (`~d`), transcription (`transcribe(d)`), and loop constructs over sequences.

### Project Files & Directory Structure

```text
CC_proj/
├── include/
│   ├── ast.h              # Abstract Syntax Tree nodes & runtime Value structures
│   ├── symbol_table.h     # Nested scope management & variable resolution
│   ├── semantics.h        # Semantic Analyzer & biological rule validator
│   └── interpreter.h      # Phase 1: In-memory AST Tree-Walk Interpreter
├── src/
│   ├── lexer.l            # Flex lexical scanner (tokens, literals, comments)
│   ├── parser.y           # Bison LALR(1) grammar specification
│   ├── semantics.cpp      # Biological alphabet validation & type checker
│   ├── interpreter.cpp    # Direct AST execution engine
│   └── main.cpp           # Main compiler / interpreter CLI driver
├── tests/
│   ├── sample1_filter.gene     # Proposal Sample 1: GC filter, ~d, transcribe(d)
│   ├── sample2_mergesort.gene  # Proposal Sample 2: Mutual recursion merge sort
│   └── error.gene              # Proposal Sample 3: Biology compile-time error
├── bin/
│   └── genescript         # Compiled executable (also symlinked as ./genec)
└── Makefile               # Standard Linux build automation
```

---

## 2. Dependencies & Installation (Linux / Ubuntu / WSL)

The following packages are required on a Linux system:
- `build-essential` (`gcc`, `g++`)
- `flex` (v2.6+)
- `bison` (v3.8+)
- `clang` & `llvm` (v16+)
- `make`

To install on Ubuntu / Debian:
```bash
sudo apt update && sudo apt install -y build-essential flex bison clang llvm make
```

---

## 3. How to Compile

Run `make` from the project root directory:
```bash
make
```

To clean previous build artifacts:
```bash
make clean
```

---

## 4. How to Run the Program

You can run GeneScript using either `./bin/genescript` or `./genec`:

### Running Sample 1 (Filter G/C bases, reverse complement, transcription)
```bash
./genec tests/sample1_filter.gene
```
**Expected Output:**
```text
GGCCG
TACAATGGCCAT
AUGGCCAUUGUA
```

### Running Sample 2 (Recursive Merge Sort)
```bash
./genec tests/sample2_mergesort.gene
```
**Expected Output:**
```text
AAACGTT
```

### Running Biological Error Detection
```bash
./genec tests/error.gene
```
**Expected Output:**
```text
tests/error.gene:1: invalid base 'X' in DNA literal.
              DNA allows only A, C, G, T.
Compilation stopped: 1 error.
```

---

## 5. Automated Test Suite

To run all automated verification tests at once:
```bash
make test
```
