/*
   Copyright 2012 Google Inc. All Rights Reserved.
   Author: jplev@google.com (John Plevyak)

   A lexical analyzer for a GraQL test/query parser.
*/

%option 8bit warn nodefault yylineno nounistd nounput
%option noyywrap

whitespace                [ \t\v\f\r]
newline                   [\n]
integer                   [0-9][0-9]*|(0[xX][0-9a-fA-F]+)
decimal                   [1-9][0-9]*[.][0-9]+
identifier                [A-Za-z_][A-Za-z_0-9]*
variable                  \?([A-Za-z_][A-Za-z_0-9]*)?
variableequals            {variable}{whitespace}*=[^=]
label                     _:([A-Za-z_][A-Za-z_0-9]*)?
key                       \/([/A-Za-z_0-9]+)
doublequotedtext          \"([^\\\"]|(\\.))*\"
singlequotedtext          \'([^\\\']|(\\.))*\'
backquotedtext            \`([^\\\`]|(\\.))*\`

cs_start              \/\*
cs_safe               [^*]
cs_may_end            \*
cs_not_may_or_end     [^/*]
cs_end                \/
cs_embed_star         ({cs_safe}*{cs_may_end}+{cs_not_may_or_end})*

cs_comment_begin      {cs_start}{cs_embed_star}{cs_safe}*{cs_may_end}*
cs_comment            {cs_start}{cs_embed_star}{cs_safe}*{cs_may_end}+{cs_end}
dash_comment          \-\-(([ \t\r].*)|\n)

%{
#define GRAQL_LEX_CC_

#include <stdio.h>
#include <string>
#include "base/logging.h"
#include "base/stringprintf.h"
#include "graph/smalld/graql/graql.h"
#include "graph/smalld/graql/graql.tab.hh"

int open_level = 0;
int yycolumn = 1;
extern YYLTYPE yylloc;
extern YYSTYPE yylval;
#define YY_USER_INIT yylineno = 1;
#define YY_DECL int yylex(YYSTYPE* yylval, YYLTYPE* yylloc, GraQLParseContext* parse_context)
#define YY_USER_ACTION yylloc->first_line = yylloc->last_line = yylineno; \
 yylloc->first_column = yycolumn; yylloc->last_column = yycolumn + yyleng - 1; \
 yycolumn += yyleng; \
 yylval->str = new string(yytext, yyleng);
#undef yywrap
#define yywrap() 1
%}
%%
%{
  if (start_token) {
    int t = start_token;
    start_token = 0;
    return t;
  }
%}


{newline} {
  yycolumn = 1;
  if (interactive && !open_level) return 0;
  /* ignore */
}
{whitespace}              { /* ignore */ }
{cs_comment}              { /* ignore */ }
{dash_comment}            { /* ignore */ }

(?i:default) { return T_DEFAULT; }
(?i:data) { return T_DATA; }
(?i:author) { return T_AUTHOR; }
(?i:subgraph) { return T_SUBGRAPH; }
(?i:set) { return T_SET; }
(?i:insert) { return T_INSERT; }
(?i:delete) { return T_DELETE; }
(?i:assume) { return T_ASSUME; }
(?i:select) { return T_SELECT; }
(?i:lookup) { return T_LOOKUP; }
(?i:verify) { return T_VERIFY; }
(?i:rule) { return T_RULE; }
(?i:function) { return T_FUNCTION; }
(?i:test) { return T_TEST; }
(?i:where) { return T_WHERE; }
(?i:from) { return T_FROM; }
(?i:into) { return T_INTO; }
(?i:in) { return T_IN; }
(?i:graph) { return T_GRAPH; }
(?i:table) { return T_TABLE; }
(?i:not) { return T_NOT; }
(?i:and) { return T_AND; }
(?i:or) { return T_OR; }
(?i:if) { return T_IF; }
(?i:then) { return T_THEN; }
(?i:else) { return T_ELSE; }
(?i:let) { return T_LET; }
(?i:as) { return T_AS; }
(?i:list) { return T_LIST; }
(?i:of) { return T_OF; }
(?i:implies) { return T_IMPLIES; }
(?i:limit) { return T_LIMIT; }
(?i:offset) { return T_OFFSET; }
(?i:by) { return T_BY; }
(?i:integer) { return T_INTEGER; }
(?i:real) { return T_REAL; }
(?i:string) { return T_STRING; }
(?i:distinct) { return T_DISTINCT; }
(?i:boolean) { return T_BOOLEAN; }
(?i:quit) { return T_QUIT; }
"||" { return T_LOR; }
"&&" { return T_LAND; }
"<=" { return T_LE; }
"==" { return T_EQ; }
">=" { return T_GE; }
"!=" { return T_NE; }
"<<" { return T_LSHIFT; }
">>" { return T_RSHIFT; }
"<" { return T_LT; }
">" { return T_GT; }
"=" { return T_EQ; }
"++" { return T_PLUSPLUS; }
"+" { return T_PLUS; }
"-" { return T_MINUS; }
"*" { return T_MULT; }
"/" { return T_DIV; }
"%" { return T_MOD; }
"!" { return T_LNOT; }
"&" { return T_BAND; }
"|" { return T_BOR; }
"^" { return T_BXOR; }
"~" { return T_MATCH; }
":" { return T_COLON; }
"@" { return T_AT; }
"{" { open_level++; return T_OPEN_BRACE; }
"}" { open_level--; return T_CLOSE_BRACE; }
"(" { open_level++; return T_OPEN_PAREN; }
")" { open_level--; return T_CLOSE_PAREN; }
"[" { open_level++; return T_OPEN_BRACKET; }
"]" { open_level--; return T_CLOSE_BRACKET; }
{decimal} { return T_DECIMAL; }
(?i:true) { return T_TRUE; }
(?i:false) { return T_FALSE; }
{integer} { return T_INT; }
{doublequotedtext} { return T_TEXT; }
{variableequals} { return T_VAREQUALS; }
{variable} { return T_VAR; }
{label} { return T_LABEL; }
{key} { return T_KEY; }
{identifier} { return T_IDENT; }

.                         {
      /* catchall rule */
      return *yytext;
}
