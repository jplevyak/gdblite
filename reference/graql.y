// Copyright 2012 Google Inc. All Rights Reserved.
// Author: jplev@google.com (John Plevyak)
//
// Grammar for a textual representation of GraQL.

%{
#include <string>
#include <iostream>

#include "base/integral_types.h"
#include "graph/smalld/graql/graql.h"
#include "strings/join.h"

#define YYDEBUG 1

// TODO(jplev): document sementics
// TODO(jplev): make it handles all of GraQL

%};

%defines
%debug
%error-verbose
%pure-parser
%locations
%debug
%expect 0
%locations

%parse-param { GraQLParseContext *context }
%lex-param { GraQLParseContext *context }

%union {
  bool bool_val;
  double double_val;
  int64 int64_val;
  string *str;
}

%token T_START_INTERACTIVE T_START_BATCH T_QUIT
%token T_DEFAULT
%token T_DATA T_AUTHOR T_SUBGRAPH
%token T_SET T_INSERT T_DELETE T_ASSUME T_SELECT T_LOOKUP T_VERIFY T_TEST
%token T_WHERE T_FROM T_INTO T_IN
%token T_GRAPH T_TABLE
%token T_FUNCTION T_RULE
%token T_NOT T_AND T_OR
%token T_IF T_THEN T_ELSE
%token T_LET T_AS T_LIST
%token T_LOR T_LAND T_LSHIFT T_RSHIFT T_MATCH
%token T_BAND T_BXOR T_BOR
%token T_DIV T_MULT T_LT T_GT T_MINUS T_PLUS T_MOD T_LNOT T_PLUSPLUS
%token T_OF T_IMPLIES T_LIMIT T_OFFSET T_BY
%token T_LABEL T_LE T_EQ T_GE T_NE T_COLON
%token T_DISTINCT T_AT
%token T_TRUE T_FALSE T_INTEGER T_STRING T_REAL T_BOOLEAN T_KEY T_IDENT
%token T_VAR T_VAREQUALS
%token T_DECIMAL T_INT T_TEXT
%token T_OPEN_PAREN T_CLOSE_PAREN T_OPEN_BRACE T_CLOSE_BRACE
%token T_OPEN_BRACKET T_CLOSE_BRACKET

%{
#define yylex graqllex
int yylex(YYSTYPE* yylval, YYLTYPE* yylloc, GraQLParseContext* parse_context);
void yyerror(YYLTYPE* locp, GraQLParseContext* parse_context,
             const string& msg);
bool interactive = false;
bool interactive_done = false;
int start_token = T_START_BATCH;
void StartInteractive() {
  interactive = true;
  start_token = T_START_INTERACTIVE;
}
void StartBatch() {
  interactive = false;
  start_token = T_START_BATCH;
}
%}

%%

start: T_START_INTERACTIVE interactive
     | T_START_BATCH batch;

batch:
     | batch stmt;

interactive
     :
     | stmt { return 0; }
     | T_QUIT { interactive_done = true; return 0; };

stmt: T_DEFAULT template
    | T_DATA template
    | T_AUTHOR thing
    | T_SUBGRAPH thing
    | T_RULE T_IDENT T_OPEN_PAREN nodes T_CLOSE_PAREN template
    | T_FUNCTION T_IDENT T_OPEN_PAREN expr_list T_CLOSE_PAREN expr
    | T_SET T_IDENT T_EQ thing
    | T_INSERT template opt_where
    | T_INSERT template T_INTO graph_name opt_where
    | T_DELETE template opt_where
    | T_DELETE template T_FROM graph_name opt_where
    | T_ASSUME template opt_where
    | T_ASSUME template T_IN graph_name opt_where
    | T_TEST opt_description test_expr test_expr
    | select
    | lookup
    | verify;

select: T_SELECT expr_as_list opt_by opt_where
      | T_SELECT expr_as_list opt_by T_FROM graph opt_where;

lookup: T_LOOKUP pattern opt_where
      | T_LOOKUP pattern T_IN graph opt_where;

verify: T_VERIFY pattern opt_where
      | T_VERIFY pattern T_IN graph opt_where;

opt_where: | T_WHERE pattern;
opt_by: | T_BY expr_list;

graph: graph_name
     | T_OPEN_PAREN lookup T_CLOSE_PAREN
     | T_OPEN_BRACE subjects T_CLOSE_BRACE
     | T_IDENT T_COLON graph;

thing: expr | table_expr | graph_expr;

graph_expr: T_GRAPH graph_name
          | lookup
          | T_OPEN_BRACE subjects T_CLOSE_BRACE
          | T_IDENT T_COLON graph_expr;

table_expr: T_TABLE T_IDENT
          | T_TABLE T_STRING
          | select;

test_expr: table_expr | lookup | T_GRAPH graph;

graph_name: T_IDENT | T_TEXT;

expr_as_list: opt_distinct_expr_as | expr_as_list ',' opt_distinct_expr_as;
opt_distinct_expr_as: opt_distinct expr_as;
opt_distinct: | T_DISTINCT;
expr_as: expr
       | expr T_AS T_IDENT
       | expr T_AS T_OPEN_PAREN ident_list T_CLOSE_PAREN;
ident_list: T_IDENT | ident_list ',' T_IDENT;

opt_description: | T_TEXT;

template: T_GRAPH graph
        | T_OPEN_PAREN lookup T_CLOSE_PAREN
        | subjects
        | T_OPEN_BRACE T_CLOSE_BRACE
        | T_IDENT T_COLON template;

pattern: T_GRAPH graph
       | defaults projection;

projection: opt_bindings exclusion;

opt_bindings: | T_LET bindings T_IN;

binding: T_VAR | T_VAREQUALS expr;
bindings: binding
        | binding ',' bindings;

exclusion: disjunction
         | exclusion T_ELSE disjunction;

disjunction: implication
           | disjunction T_OR implication;

implication: conjunction
           | implication T_IMPLIES conjunction;

conjunction: basic
           | conjunction T_AND basic;

basic: subjects
     | T_VAREQUALS expr
     | T_OPEN_PAREN pattern T_CLOSE_PAREN;

subjects: subject
        | T_OPEN_BRACE subjects T_CLOSE_BRACE
        | subject '.'
        | subject '.' subjects;

subject: call
       | T_NOT node predicates
       | node predicate_phrase
       ;

predicate_phrase: predicates
                | T_GT node
                | T_LT node
                | T_GE node
                | T_LE node
                | T_EQ node
                | T_NE node
                ;

predicates: predicateless_modifiers
          | anode opt_transitive objects modifiers more_predicates
          | T_LSHIFT anode opt_transitive objects modifiers more_predicates
          | T_RSHIFT anode opt_transitive objects modifiers more_predicates
          | T_LNOT anode opt_transitive objects modifiers more_predicates
          | T_NOT anode opt_transitive objects modifiers more_predicates
          ;

opt_transitive: | T_PLUSPLUS;

more_predicates:
               | T_LSHIFT anode objects modifiers more_predicates
               | T_RSHIFT anode objects modifiers more_predicates
               | ';' predicates
               ;

anode: 'a' | node;

objects:
       | object
       | object author
       | object author subgraph
       | object ',' objects
       ;

author: node;
subgraph: node;

defaults:
        | T_PLUS defaults
        | T_MINUS defaults
        | T_MULT defaults
        | T_DIV defaults;

modifiers: predicateless_modifiers
         | condense;

predicateless_modifiers:
         | T_PLUS modifiers /* must include */
         | T_MINUS modifiers /* optional */
         | T_MULT modifiers /* return */
         | T_DIV modifiers /* / */
         | T_LIMIT T_INT modifiers
         | T_OFFSET T_INT modifiers
         ;

condense: T_LNOT
        | T_LNOT T_AT node;

object: node | T_NOT node | T_OPEN_PAREN opt_variable expr T_CLOSE_PAREN;

node: T_VAR | T_KEY | T_IDENT | blank | literal;

blank: labelled | anonymous | list;

labelled: T_LABEL;

anonymous: T_OPEN_BRACKET opt_variable predicate_phrase T_CLOSE_BRACKET;

opt_variable: | T_COLON T_VAR;

nodes: | node nodes;

list: T_LIST T_OPEN_PAREN nodes T_CLOSE_PAREN;

call : T_IDENT T_OPEN_PAREN nodes T_CLOSE_PAREN
     | T_IDENT T_OPEN_PAREN nodes T_CLOSE_PAREN T_AT node;

expr: T_IF disjunction T_THEN disjunctive T_ELSE expr
    | disjunctive;

disjunctive: conjunctive
           | disjunctive T_LOR conjunctive
           | T_LOR conjunctive;

conjunctive: equality
           | conjunctive T_LAND equality
           | T_LAND equality;

equality: relational
        | equality T_MATCH relational
        | T_MATCH relational
        | equality T_EQ opt_type relational
        | T_EQ opt_type relational
        | equality T_NE opt_type relational
        | T_NE opt_type relational;

relational: shiftive
         | relational T_LT opt_type shiftive
         | T_LT opt_type shiftive
         | relational T_GT opt_type shiftive
         | T_GT opt_type shiftive
         | relational T_LE opt_type shiftive
         | T_LE opt_type shiftive
         | relational T_GE opt_type shiftive
         | T_GE opt_type shiftive;

shiftive: additive
        | shiftive T_LSHIFT opt_type additive
        | shiftive T_RSHIFT opt_type additive
        ;

additive: multiplicative
        | additive T_PLUS opt_type multiplicative
        | additive T_MINUS opt_type multiplicative;

multiplicative: bitop
              | multiplicative T_MULT opt_type bitop
              | multiplicative T_DIV opt_type bitop
              | multiplicative T_MOD opt_type bitop;

bitop: unary
     | bitop T_BAND unary
     | bitop T_BOR unary
     | bitop T_BXOR unary;

unary: T_LNOT unary
     | T_PLUS opt_type unary
     | T_MINUS opt_type unary
     | primary;

primary: T_VAR
       | T_KEY
       | T_IDENT
       | literal
       | evaluation
       | sequence
       | T_OPEN_PAREN expr T_CLOSE_PAREN;

evaluation: T_IDENT T_OPEN_PAREN expr_list T_CLOSE_PAREN
          | T_IDENT T_OF sequence;

sequence: T_OPEN_BRACKET expr_list opt_where T_CLOSE_BRACKET;

expr_list: expr | expr_list ',' expr;

opt_type: | T_INTEGER | T_REAL | T_STRING | T_BOOLEAN;

literal: T_TRUE | T_FALSE | T_INT | T_DECIMAL | T_TEXT;

%%

void yyerror(YYLTYPE* locp, GraQLParseContext* parse_context,
    const string& msg) {
  cout << "GraQL Parser Error, line " << locp->first_line;
  if (locp->first_line != locp->last_line)
    cout << "-" << locp->last_line;
  if (locp->first_column != locp->last_column) {
    cout << " columns " << locp->first_column;
    cout << "-" << locp->last_column;
  } else {
    cout << " column " << locp->first_column;
  }
  cout << ": " << msg << "\n";
}
