%{
#include "ast.h"
#include "parser.tab.h"
#include <string>
#include <cstdlib>

extern int yylineno;
int yycolumn = 1;

#define YY_USER_ACTION \
    yylloc.first_line = yylineno; \
    yylloc.first_column = yycolumn; \
    yycolumn += yyleng;
%}

%option noyywrap
%option yylineno
%option nounput
%option noinput

%%

[ \t\r]+                { /* ignore whitespace */ }
\n                      { yycolumn = 1; }
\^\^[^\n]*              { /* comment ^^ ignores until end of line */ }

"otherwise"[ \t\r\n]+"when" { return OTHERWISE_WHEN; }
"otherwise"             { return OTHERWISE; }
"when"                  { return WHEN; }
"stop"                  { return STOP; }
"gene"                  { return GENE; }
"express"               { return EXPRESS; }
"report"                { return REPORT; }
"dna"                   { return TYPE_DNA_KW; }
"rna"                   { return TYPE_RNA_KW; }
"cycle"                 { return CYCLE; }
"across"                { return ACROSS; }
"from"                  { return FROM; }
"to"                    { return TO; }
"grow"                  { return GROW; }
"until"                 { return UNTIL; }
"halt"                  { return HALT; }
"skip"                  { return SKIP; }
"both"                  { return BOTH; }
"either"                { return EITHER; }
"lacks"                 { return LACKS; }
"bound"                 { return BOUND; }
"unbound"               { return UNBOUND; }
"fuse"                  { return FUSE; }
"head"                  { return HEAD; }
"tail"                  { return TAIL; }
"transcribe"            { return TRANSCRIBE; }

"->"                    { return ARROW; }
"="                     { return EQ; }
"<="                    { return LE; }
">="                    { return GE; }
"<"                     { return LT; }
">"                     { return GT; }
"~"                     { return TILDE; }
"@"                     { return AT; }
"#"                     { return HASH; }
"("                     { return LPAREN; }
")"                     { return RPAREN; }
","                     { return COMMA; }

\"[^\"]*\"              {
                            // Strip double quotes
                            std::string s(yytext + 1, yyleng - 2);
                            yylval.str_val = strdup(s.c_str());
                            return STRING_LITERAL;
                        }

\'[^\']\'               {
                            // Strip single quotes
                            std::string s(yytext + 1, 1);
                            yylval.str_val = strdup(s.c_str());
                            return BASE_LITERAL;
                        }

[0-9]+                  {
                            yylval.int_val = std::atoll(yytext);
                            return INT_LITERAL;
                        }

[a-zA-Z_][a-zA-Z0-9_]*  {
                            yylval.str_val = strdup(yytext);
                            return ID;
                        }

.                       {
                            fprintf(stderr, "Lexical Error at line %d: unrecognized character '%s'\n", yylineno, yytext);
                        }

%%
