CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Iinclude
FLEX = flex
BISON = bison

SRCS = src/semantics.cpp src/interpreter.cpp src/main.cpp
OBJS = $(SRCS:.cpp=.o) src/parser.tab.o src/lex.yy.o
TARGET = bin/genescript

all: $(TARGET)

pdf:
	python3 generate_pdf.py

$(TARGET): src/parser.tab.o src/lex.yy.o $(OBJS) | bin
	$(CXX) $(CXXFLAGS) -o $@ src/parser.tab.o src/lex.yy.o $(SRCS:.cpp=.o)
	cp $(TARGET) ./genec 2>/dev/null || true

bin:
	mkdir -p bin

src/parser.tab.c src/parser.tab.h: src/parser.y
	$(BISON) -d -o src/parser.tab.c src/parser.y

src/lex.yy.c: src/lexer.l src/parser.tab.h
	$(FLEX) -o src/lex.yy.c src/lexer.l

src/parser.tab.o: src/parser.tab.c
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/lex.yy.o: src/lex.yy.c
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.cpp
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

clean:
	rm -rf bin src/parser.tab.c src/parser.tab.h src/lex.yy.c src/*.o genec

.PHONY: all clean test
