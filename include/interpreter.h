#ifndef GENESCRIPT_INTERPRETER_H
#define GENESCRIPT_INTERPRETER_H

#include "ast.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <stdexcept>

class InterpreterContext {
public:
    std::unordered_map<std::string, GeneDeclAST*> genes;
    std::vector<std::unordered_map<std::string, Value>> frames;

    InterpreterContext() {
        pushFrame(); // Global frame
    }

    void pushFrame() {
        frames.emplace_back();
    }

    void popFrame() {
        if (!frames.empty()) {
            frames.pop_back();
        }
    }

    void setVar(const std::string& name, const Value& val) {
        if (!frames.empty()) {
            frames.back()[name] = val;
        }
    }

    bool assignVar(const std::string& name, const Value& val) {
        for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
            auto fit = it->find(name);
            if (fit != it->end()) {
                fit->second = val;
                return true;
            }
        }
        // If not found in any existing frame, define in current frame
        setVar(name, val);
        return true;
    }

    Value getVar(const std::string& name) {
        for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
            auto fit = it->find(name);
            if (fit != it->end()) {
                return fit->second;
            }
        }
        throw std::runtime_error("Runtime Error: Undefined variable '" + name + "'");
    }

    Value callGene(const std::string& name, const std::vector<Value>& arg_values);
    void executeProgram(ProgramAST& program);
};

#endif // GENESCRIPT_INTERPRETER_H
