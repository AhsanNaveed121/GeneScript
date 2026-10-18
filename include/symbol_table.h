#ifndef GENESCRIPT_SYMBOL_TABLE_H
#define GENESCRIPT_SYMBOL_TABLE_H

#include "ast.h"
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

struct Symbol {
    std::string name;
    DataType type;
    bool is_counter; // Loop counter (read-only)
    SourceLoc loc;
};

class Scope {
public:
    std::unordered_map<std::string, Symbol> symbols;
    std::shared_ptr<Scope> parent;

    Scope(std::shared_ptr<Scope> parent = nullptr) : parent(parent) {}

    bool declare(const Symbol& sym) {
        if (symbols.find(sym.name) != symbols.end()) {
            return false; // Already declared in this scope
        }
        symbols[sym.name] = sym;
        return true;
    }

    Symbol* lookup(const std::string& name) {
        auto it = symbols.find(name);
        if (it != symbols.end()) return &it->second;
        if (parent) return parent->lookup(name);
        return nullptr;
    }
};

#endif // GENESCRIPT_SYMBOL_TABLE_H
