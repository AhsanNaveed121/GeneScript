CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Iinclude
FLEX = flex
BISON = bison

# Shared front end: lexer + parser + semantic analysis (+ interpreter, which
# also holds the AST vtables every binary needs).
FRONT_OBJS = src/parser.tab.o src/lex.yy.o src/semantics.o src/interpreter.o

SRCS = src/semantics.cpp src/interpreter.cpp src/main.cpp
OBJS = $(SRCS:.cpp=.o) src/parser.tab.o src/lex.yy.o
# Every object is rebuilt when a header changes (class layouts are shared).
HEADERS = $(wildcard include/*.h) runtime/gs_runtime.h
TARGET = bin/genescript
GENEC_TARGET = bin/genec
LLVM_TARGET = bin/genescript-llvm
CLANG ?= clang

# `make` builds both phases; `make interpreter` / `make compiler` build one.
#   bin/genescript       Phase 1 interpreter only (no LLVM code linked in)
#   bin/genec, ./genec   full CLI: interpret, -S (LLVM IR), -c (native via clang)
#   bin/genescript-llvm  Phase 2 development driver used by tests/llvm
all: interpreter compiler

interpreter: $(TARGET)

compiler: $(GENEC_TARGET) $(LLVM_TARGET)

pdf:
	python3 generate_pdf.py

$(TARGET): src/parser.tab.o src/lex.yy.o $(OBJS) | bin
	$(CXX) $(CXXFLAGS) -o $@ src/parser.tab.o src/lex.yy.o $(SRCS:.cpp=.o)

bin:
	mkdir -p bin

src/parser.tab.c src/parser.tab.h: src/parser.y
	$(BISON) -d -o src/parser.tab.c src/parser.y

src/lex.yy.c: src/lexer.l src/parser.tab.h
	$(FLEX) -o src/lex.yy.c src/lexer.l

src/parser.tab.o: src/parser.tab.c $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/lex.yy.o: src/lex.yy.c $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: $(TARGET)
	@echo "=========================================="
	@echo "Running Test 1: GC Filter, Reverse Comp, Transcribe"
	@echo "=========================================="
	@$(TARGET) tests/sample1_filter.gene
	@echo ""
	@echo "=========================================="
	@echo "Running Test 2: Recursive Merge Sort"
	@echo "=========================================="
	@$(TARGET) tests/sample2_mergesort.gene
	@echo ""
	@echo "=========================================="
	@echo "Running Test 3: Biological Error Detection"
	@echo "=========================================="
	@-$(TARGET) tests/error.gene
	@echo "=========================================="
	@echo "Running Bug-Fix Tests (tests/fix*.gene vs tests/expected/)"
	@echo "=========================================="
	@sh tests/run_tests.sh $(TARGET)

# ---- Phase 2: AOT compiler ---------------------------------------------------
# Building needs only g++; running `genec -c` and the compiler tests needs clang.
$(GENEC_TARGET): $(FRONT_OBJS) src/codegen.o src/genec.o | bin
	$(CXX) $(CXXFLAGS) -o $@ $(FRONT_OBJS) src/codegen.o src/genec.o
	cp $(GENEC_TARGET) ./genec

$(LLVM_TARGET): $(FRONT_OBJS) src/codegen.o src/llvm_main.o | bin
	$(CXX) $(CXXFLAGS) -o $@ $(FRONT_OBJS) src/codegen.o src/llvm_main.o

llvm: $(LLVM_TARGET)

# Backend unit/IR tests (Parts 1-3) and the end-to-end genec tests (Part 4).
test-compiler: all
	@CLANG="$(CLANG)" sh tests/llvm/run_llvm_tests.sh $(TARGET) $(LLVM_TARGET)
	@echo ""
	@CLANG="$(CLANG)" sh tests/compiler/run_compiler_tests.sh ./genec

test-llvm: $(TARGET) $(LLVM_TARGET)
	@CLANG="$(CLANG)" sh tests/llvm/run_llvm_tests.sh $(TARGET) $(LLVM_TARGET)

clean:
	rm -rf bin src/parser.tab.c src/parser.tab.h src/lex.yy.c src/*.o genec

.PHONY: all interpreter compiler clean test llvm test-llvm test-compiler
