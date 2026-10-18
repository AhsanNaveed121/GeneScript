%{
#include "ast.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstring>

extern int yylex();
extern int yylineno;
extern char* yytext;
void yyerror(const char* s);

ProgramAST* g_program = nullptr;

struct CondBranchRaw {
    ExprAST* condition;
    std::vector<StmtAST*>* body;
};

%}

%locations
%expect 1

%union {
    char* str_val;
    long long int_val;
    int data_type;
    class ExprAST* expr;
    class StmtAST* stmt;
    class GeneDeclAST* gene;
    std::vector<class StmtAST*>* stmt_list;
    std::vector<class ExprAST*>* expr_list;
    std::vector<struct ParamDecl>* param_list;
    std::vector<struct CondBranchRaw>* branch_list;
}

%token <str_val> STRING_LITERAL BASE_LITERAL ID
%token <int_val> INT_LITERAL
%token TYPE_DNA_KW TYPE_RNA_KW
%token GENE EXPRESS REPORT
%token WHEN OTHERWISE OTHERWISE_WHEN STOP
%token CYCLE ACROSS FROM TO GROW UNTIL HALT SKIP
%token BOTH EITHER LACKS BOUND UNBOUND
%token FUSE HEAD TAIL TRANSCRIBE
%token ARROW EQ LE GE LT GT TILDE AT HASH LPAREN RPAREN COMMA

%type <data_type> type_spec
%type <expr> expr primary_expr call_expr
%type <stmt> stmt
%type <stmt_list> stmt_list
%type <gene> gene_decl
%type <param_list> param_list param_list_opt
%type <expr_list> arg_list arg_list_opt
%type <branch_list> otherwise_when_chain
%type <stmt_list> otherwise_opt

%left EITHER
%left BOTH
%nonassoc EQ LT GT LE GE
%right FUSE
%right TILDE LACKS HASH
%left AT

%start program

%%

program:
    item_list
    ;

item_list:
    /* empty */
    | item_list gene_decl
      {
          if (!g_program) g_program = new ProgramAST();
          g_program->genes.emplace_back($2);
      }
    | item_list stmt
      {
          if (!g_program) g_program = new ProgramAST();
          g_program->top_level_stmts.emplace_back($2);
      }
    ;

type_spec:
    TYPE_DNA_KW { $$ = TYPE_DNA; }
    | TYPE_RNA_KW { $$ = TYPE_RNA; }
    ;

gene_decl:
    GENE ID LPAREN param_list_opt RPAREN stmt_list STOP
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        std::vector<ParamDecl> params;
        if ($4) {
            params = *$4;
            delete $4;
        }
        std::vector<std::unique_ptr<StmtAST>> body;
        if ($6) {
            for (auto* s : *$6) {
                body.emplace_back(s);
            }
            delete $6;
        }
        $$ = new GeneDeclAST(loc, $2, params, std::move(body));
        free($2);
    }
    ;

param_list_opt:
    /* empty */ { $$ = new std::vector<ParamDecl>(); }
    | param_list { $$ = $1; }
    ;

param_list:
    type_spec ID
    {
        $$ = new std::vector<ParamDecl>();
        ParamDecl p;
        p.type = (DataType)$1;
        p.name = $2;
        p.loc = { @2.first_line, @2.first_column };
        $$->push_back(p);
        free($2);
    }
    | param_list COMMA type_spec ID
    {
        $$ = $1;
        ParamDecl p;
        p.type = (DataType)$3;
        p.name = $4;
        p.loc = { @4.first_line, @4.first_column };
        $$->push_back(p);
        free($4);
    }
    ;

stmt_list:
    /* empty */ { $$ = new std::vector<StmtAST*>(); }
    | stmt_list stmt
    {
        $$ = $1;
        if ($2) $$->push_back($2);
    }
    ;

stmt:
    expr ARROW type_spec ID
    {
        SourceLoc loc = { @2.first_line, @2.first_column };
        $$ = new DeclStmtAST(loc, std::unique_ptr<ExprAST>($1), (DataType)$3, $4);
        free($4);
    }
    | expr ARROW ID
    {
        SourceLoc loc = { @2.first_line, @2.first_column };
        $$ = new AssignStmtAST(loc, std::unique_ptr<ExprAST>($1), $3);
        free($3);
    }
    | REPORT expr
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        $$ = new ReportStmtAST(loc, std::unique_ptr<ExprAST>($2));
    }
    | EXPRESS expr
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        $$ = new ExpressStmtAST(loc, std::unique_ptr<ExprAST>($2));
    }
    | WHEN expr stmt_list otherwise_when_chain otherwise_opt STOP
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        WhenStmtAST* w = new WhenStmtAST(loc);
        
        // Primary when branch
        CondBranch main_branch;
        main_branch.condition = std::unique_ptr<ExprAST>($2);
        if ($3) {
            for (auto* s : *$3) main_branch.body.emplace_back(s);
            delete $3;
        }
        w->branches.push_back(std::move(main_branch));

        // otherwise when branches
        if ($4) {
            for (auto& b : *$4) {
                CondBranch cb;
                cb.condition = std::unique_ptr<ExprAST>(b.condition);
                if (b.body) {
                    for (auto* s : *b.body) cb.body.emplace_back(s);
                    delete b.body;
                }
                w->branches.push_back(std::move(cb));
            }
            delete $4;
        }

        // otherwise branch
        if ($5) {
            for (auto* s : *$5) w->otherwise_body.emplace_back(s);
            delete $5;
        }

        $$ = w;
    }
    | CYCLE ID ACROSS expr stmt_list STOP
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        std::vector<std::unique_ptr<StmtAST>> body;
        if ($5) {
            for (auto* s : *$5) body.emplace_back(s);
            delete $5;
        }
        $$ = new CycleAcrossStmtAST(loc, $2, std::unique_ptr<ExprAST>($4), std::move(body));
        free($2);
    }
    | CYCLE ID FROM expr TO expr stmt_list STOP
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        std::vector<std::unique_ptr<StmtAST>> body;
        if ($7) {
            for (auto* s : *$7) body.emplace_back(s);
            delete $7;
        }
        $$ = new CycleFromToStmtAST(loc, $2, std::unique_ptr<ExprAST>($4), std::unique_ptr<ExprAST>($6), std::move(body));
        free($2);
    }
    | GROW UNTIL expr stmt_list STOP
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        std::vector<std::unique_ptr<StmtAST>> body;
        if ($4) {
            for (auto* s : *$4) body.emplace_back(s);
            delete $4;
        }
        $$ = new GrowUntilStmtAST(loc, std::unique_ptr<ExprAST>($3), std::move(body));
    }
    | HALT
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        $$ = new HaltStmtAST(loc);
    }
    | SKIP
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        $$ = new SkipStmtAST(loc);
    }
    ;

otherwise_when_chain:
    /* empty */ { $$ = new std::vector<CondBranchRaw>(); }
    | otherwise_when_chain OTHERWISE_WHEN expr stmt_list
    {
        $$ = $1;
        CondBranchRaw b;
        b.condition = $3;
        b.body = $4;
        $$->push_back(b);
    }
    ;

otherwise_opt:
    /* empty */ { $$ = nullptr; }
    | OTHERWISE stmt_list { $$ = $2; }
    ;

expr:
    primary_expr { $$ = $1; }
    | FUSE expr expr
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        $$ = new BinaryExprAST(loc, OP_FUSE, std::unique_ptr<ExprAST>($2), std::unique_ptr<ExprAST>($3));
    }
    | expr EQ expr
    {
        SourceLoc loc = { @2.first_line, @2.first_column };
        $$ = new BinaryExprAST(loc, OP_EQ, std::unique_ptr<ExprAST>($1), std::unique_ptr<ExprAST>($3));
    }
    | expr LT expr
    {
        SourceLoc loc = { @2.first_line, @2.first_column };
        $$ = new BinaryExprAST(loc, OP_LT, std::unique_ptr<ExprAST>($1), std::unique_ptr<ExprAST>($3));
    }
    | expr GT expr
    {
        SourceLoc loc = { @2.first_line, @2.first_column };
        $$ = new BinaryExprAST(loc, OP_GT, std::unique_ptr<ExprAST>($1), std::unique_ptr<ExprAST>($3));
    }
    | expr LE expr
    {
        SourceLoc loc = { @2.first_line, @2.first_column };
        $$ = new BinaryExprAST(loc, OP_LE, std::unique_ptr<ExprAST>($1), std::unique_ptr<ExprAST>($3));
    }
    | expr GE expr
    {
        SourceLoc loc = { @2.first_line, @2.first_column };
        $$ = new BinaryExprAST(loc, OP_GE, std::unique_ptr<ExprAST>($1), std::unique_ptr<ExprAST>($3));
    }
    | expr BOTH expr
    {
        SourceLoc loc = { @2.first_line, @2.first_column };
        $$ = new BinaryExprAST(loc, OP_BOTH, std::unique_ptr<ExprAST>($1), std::unique_ptr<ExprAST>($3));
    }
    | expr EITHER expr
    {
        SourceLoc loc = { @2.first_line, @2.first_column };
        $$ = new BinaryExprAST(loc, OP_EITHER, std::unique_ptr<ExprAST>($1), std::unique_ptr<ExprAST>($3));
    }
    | LACKS expr
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        $$ = new UnaryExprAST(loc, OP_LACKS, std::unique_ptr<ExprAST>($2));
    }
    | TILDE expr
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        $$ = new UnaryExprAST(loc, OP_REVCOMP, std::unique_ptr<ExprAST>($2));
    }
    | HASH expr
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        $$ = new UnaryExprAST(loc, OP_LEN, std::unique_ptr<ExprAST>($2));
    }
    | expr AT expr
    {
        SourceLoc loc = { @2.first_line, @2.first_column };
        $$ = new IndexExprAST(loc, std::unique_ptr<ExprAST>($1), std::unique_ptr<ExprAST>($3));
    }
    ;

primary_expr:
    STRING_LITERAL
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        $$ = new LiteralExprAST(loc, $1, false);
        free($1);
    }
    | BASE_LITERAL
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        $$ = new LiteralExprAST(loc, $1, true);
        free($1);
    }
    | INT_LITERAL
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        $$ = new IntLiteralExprAST(loc, $1);
    }
    | BOUND
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        $$ = new BoolLiteralExprAST(loc, true);
    }
    | UNBOUND
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        $$ = new BoolLiteralExprAST(loc, false);
    }
    | ID
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        $$ = new VarExprAST(loc, $1);
        free($1);
    }
    | call_expr { $$ = $1; }
    | LPAREN expr RPAREN { $$ = $2; }
    ;

call_expr:
    HEAD LPAREN expr RPAREN
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        $$ = new BuiltinCallExprAST(loc, BUILTIN_HEAD, std::unique_ptr<ExprAST>($3));
    }
    | TAIL LPAREN expr RPAREN
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        $$ = new BuiltinCallExprAST(loc, BUILTIN_TAIL, std::unique_ptr<ExprAST>($3));
    }
    | TRANSCRIBE LPAREN expr RPAREN
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        $$ = new BuiltinCallExprAST(loc, BUILTIN_TRANSCRIBE, std::unique_ptr<ExprAST>($3));
    }
    | ID LPAREN arg_list_opt RPAREN
    {
        SourceLoc loc = { @1.first_line, @1.first_column };
        std::vector<std::unique_ptr<ExprAST>> args;
        if ($3) {
            for (auto* a : *$3) args.emplace_back(a);
            delete $3;
        }
        $$ = new CallExprAST(loc, $1, std::move(args));
        free($1);
    }
    ;

arg_list_opt:
    /* empty */ { $$ = new std::vector<ExprAST*>(); }
    | arg_list { $$ = $1; }
    ;

arg_list:
    expr
    {
        $$ = new std::vector<ExprAST*>();
        $$->push_back($1);
    }
    | arg_list COMMA expr
    {
        $$ = $1;
        $$->push_back($3);
    }
    ;

%%

void yyerror(const char* s) {
    fprintf(stderr, "Syntax Error at line %d, col %d: %s near '%s'\n", yylloc.first_line, yylloc.first_column, s, yytext);
}
