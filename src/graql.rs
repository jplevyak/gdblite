//! GraQL: a textual query language for graphs, after the C++ GraQL grammar
//! (`graql.y`), compiled to [`Query`] values.
//!
//! A pattern is written as in Turtle: a subject, then predicates and their
//! objects, `;` between predicates of the subject, `,` between objects of a
//! predicate, and `.` between subjects.
//!
//! ```
//! use gdblite::graph::{Graph, Options};
//! use gdblite::graql::{self, Output};
//!
//! let mut g = Graph::open(None, Options::default()).unwrap();
//! graql::execute(&mut g, r#"
//!     insert /alice knows /bob, /carol ; age 30 .
//!            /bob knows /carol ; age 25 .
//!            /carol age 35
//! "#).unwrap();
//! let out = graql::execute(&mut g, r#"
//!     select ?who, ?age by ?age
//!     where /alice knows ?who . ?who age ?age . ?age < 30
//! "#).unwrap();
//! let Output::Rows { rows, .. } = &out[0] else { panic!() };
//! assert_eq!(rows.len(), 1);
//! assert_eq!(g.data(rows[0][1]), Some(&b"25"[..]));
//! ```
//!
//! # Statements
//!
//! - `select [distinct] ?x, ?y | * [by ?x, ...] [limit n] where pattern`
//!   returns rows of the variables (all of them, for `*`), ordered by the
//!   data of the values of the `by` variables.  The `limit` is an addition
//!   to the C++ grammar, which has limits only on predicates.
//! - `verify pattern` returns whether the pattern has a match.
//! - `lookup pattern` returns the links its matches use.
//! - `insert template [where pattern]` links the template's triples, once,
//!   or for each match of the pattern, creating the nodes, values and keys
//!   the template names; a blank node in it is a new node each time.
//! - `delete template [where pattern]` deletes them.
//!
//! The changes are not committed.
//!
//! # Patterns
//!
//! - `?x` is a variable, `?` a new one; `_:x` and `[ predicates ]` are blank
//!   nodes, variables of their own, and `[ :?x predicates ]` names one.
//! - `/a/b` is the node with that key (see [`Graph::node_by_key`]).
//! - An identifier, a "string" or a number is the value with that data, and
//!   `a`, as a predicate, the value `type`.
//! - `p and q` both hold, `p or q` either does (both must bind the same
//!   variables), `not p` does not hold, and parentheses group.
//! - A negated subject (`not s p o`) or predicate (`s not p o`) is a
//!   negated pattern.
//! - Comparisons are subjects: `?x = ?y` and `?x != ?y` compare ids, which
//!   for values is their data; `<`, `<=`, `>` and `>=` compare data,
//!   numerically if both are numbers.
//!
//! The rest of the C++ grammar parses but is reported as unsupported:
//! optional and other predicate modifiers, transitive predicates (`++`),
//! author and subgraph positions, `let`, `implies`, `else`, expressions,
//! lists, rules, functions, named graphs and the other statements.

use std::collections::{BTreeSet, HashMap};
use std::fmt;
use std::sync::Arc;

use crate::graph::{self, Graph};
use crate::query::{Cmp, Filter, Group, Query, SortKey, Term, Var, compare_data};

/// Errors from parsing or running GraQL.
#[derive(Debug)]
pub enum Error {
    /// The text is not GraQL, at `line` and `column` (from 1).
    Syntax {
        line: usize,
        column: usize,
        message: String,
    },
    /// The text uses something not supported, at `line` and `column`.
    Unsupported {
        line: usize,
        column: usize,
        what: String,
    },
    Graph(graph::Error),
}

pub type Result<T> = std::result::Result<T, Error>;

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Syntax {
                line,
                column,
                message,
            } => write!(f, "{line}:{column}: {message}"),
            Error::Unsupported { line, column, what } => {
                write!(f, "{line}:{column}: {what} is not supported")
            }
            Error::Graph(e) => write!(f, "{e}"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Error::Graph(e) => Some(e),
            _ => None,
        }
    }
}

impl From<graph::Error> for Error {
    fn from(e: graph::Error) -> Self {
        Error::Graph(e)
    }
}

/// A token of GraQL text.
#[derive(Debug, Clone, PartialEq)]
enum Token {
    /// A keyword, in lower case.
    Keyword(&'static str),
    /// A variable's name, without the `?`; empty for `?` alone.
    Var(String),
    /// A blank node's label, without the `_:`.
    Label(String),
    /// A key, with its leading `/`.
    Key(String),
    Ident(String),
    Text(String),
    Number(String),
    Punct(&'static str),
    End,
}

impl fmt::Display for Token {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Token::Keyword(k) => write!(f, "'{k}'"),
            Token::Var(v) => write!(f, "?{v}"),
            Token::Label(l) => write!(f, "_:{l}"),
            Token::Key(k) | Token::Ident(k) | Token::Number(k) => write!(f, "{k}"),
            Token::Text(t) => write!(f, "{t:?}"),
            Token::Punct(p) => write!(f, "'{p}'"),
            Token::End => write!(f, "the end"),
        }
    }
}

const KEYWORDS: &[&str] = &[
    "default", "data", "author", "subgraph", "set", "insert", "delete", "assume", "select",
    "lookup", "verify", "rule", "function", "test", "where", "from", "into", "in", "graph",
    "table", "not", "and", "or", "if", "then", "else", "let", "as", "list", "of", "implies",
    "limit", "offset", "by", "integer", "real", "string", "distinct", "boolean", "quit", "true",
    "false",
];

/// Longest first, so that the longest punctuation matches.
const PUNCT: &[&str] = &[
    "||", "&&", "<=", "==", ">=", "!=", "<<", ">>", "++", "<", ">", "=", "+", "-", "*", "/", "%",
    "!", "&", "|", "^", "~", ":", "@", "{", "}", "(", ")", "[", "]", ".", ",", ";",
];

/// A token and where it starts.
#[derive(Debug, Clone)]
struct Spanned {
    token: Token,
    line: usize,
    column: usize,
}

fn is_ident_start(c: char) -> bool {
    c.is_ascii_alphabetic() || c == '_'
}

fn is_ident(c: char) -> bool {
    c.is_ascii_alphanumeric() || c == '_'
}

fn lex(text: &str) -> Result<Vec<Spanned>> {
    let chars: Vec<char> = text.chars().collect();
    let (mut i, mut line, mut column) = (0, 1, 1);
    let mut out = Vec::new();
    let advance = |i: &mut usize, line: &mut usize, column: &mut usize, n: usize| {
        for _ in 0..n {
            if chars[*i] == '\n' {
                *line += 1;
                *column = 1;
            } else {
                *column += 1;
            }
            *i += 1;
        }
    };
    let error = |line, column, message: String| Error::Syntax {
        line,
        column,
        message,
    };
    while i < chars.len() {
        let c = chars[i];
        let rest = &chars[i..];
        let starts = |s: &str| rest.iter().take(s.len()).copied().eq(s.chars());
        let (start_line, start_column) = (line, column);
        // Whitespace and comments.
        if c.is_whitespace() {
            advance(&mut i, &mut line, &mut column, 1);
            continue;
        }
        if starts("/*") {
            let end = (2..rest.len().saturating_sub(1))
                .find(|&k| rest[k] == '*' && rest[k + 1] == '/')
                .ok_or_else(|| error(line, column, "unterminated comment".into()))?;
            advance(&mut i, &mut line, &mut column, end + 2);
            continue;
        }
        if starts("--")
            && rest
                .get(2)
                .is_none_or(|&c| c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            let end = rest.iter().position(|&c| c == '\n').unwrap_or(rest.len());
            advance(&mut i, &mut line, &mut column, end);
            continue;
        }
        let take_while = |from: usize, f: fn(char) -> bool| {
            from + rest[from..].iter().take_while(|&&c| f(c)).count()
        };
        let (token, len) = if c == '?' {
            let end = if rest.get(1).is_some_and(|&c| is_ident_start(c)) {
                take_while(1, is_ident)
            } else {
                1
            };
            (Token::Var(rest[1..end].iter().collect()), end)
        } else if starts("_:") {
            let end = take_while(2, is_ident);
            (Token::Label(rest[2..end].iter().collect()), end)
        } else if c == '/' && rest.get(1).is_some_and(|&c| is_ident(c) || c == '/') {
            let end = take_while(1, |c| is_ident(c) || c == '/');
            (Token::Key(rest[..end].iter().collect()), end)
        } else if c == '"' {
            let mut s = String::new();
            let mut k = 1;
            loop {
                match rest.get(k) {
                    None => return Err(error(line, column, "unterminated string".into())),
                    Some('"') => break,
                    Some('\\') => {
                        let escaped = rest
                            .get(k + 1)
                            .ok_or_else(|| error(line, column, "unterminated string".into()))?;
                        s.push(match escaped {
                            'n' => '\n',
                            't' => '\t',
                            'r' => '\r',
                            &c => c,
                        });
                        k += 2;
                    }
                    Some(&c) => {
                        s.push(c);
                        k += 1;
                    }
                }
            }
            (Token::Text(s), k + 1)
        } else if c.is_ascii_digit() {
            let mut end = take_while(0, |c| c.is_ascii_alphanumeric());
            if rest.get(end) == Some(&'.') && rest.get(end + 1).is_some_and(|c| c.is_ascii_digit())
            {
                end = take_while(end + 1, |c| c.is_ascii_digit());
            }
            (Token::Number(rest[..end].iter().collect()), end)
        } else if is_ident_start(c) {
            let end = take_while(0, is_ident);
            let word: String = rest[..end].iter().collect();
            let lower = word.to_ascii_lowercase();
            match KEYWORDS.iter().find(|&&k| k == lower) {
                Some(k) => (Token::Keyword(k), end),
                None => (Token::Ident(word), end),
            }
        } else if let Some(p) = PUNCT.iter().find(|p| starts(p)) {
            (Token::Punct(p), p.len())
        } else {
            return Err(error(line, column, format!("unexpected character {c:?}")));
        };
        advance(&mut i, &mut line, &mut column, len);
        out.push(Spanned {
            token,
            line: start_line,
            column: start_column,
        });
    }
    out.push(Spanned {
        token: Token::End,
        line,
        column,
    });
    Ok(out)
}

/// A node of a pattern, before it is resolved against a graph.
#[derive(Debug, Clone, PartialEq)]
pub enum Node {
    Var(String),
    /// A blank node: a variable of its own.
    Blank(String),
    Key(String),
    /// An identifier, string or number: a value's data.
    Value(String),
}

/// A comparison between two nodes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Comparison {
    Eq,
    Ne,
    Data(Cmp),
}

/// A parsed pattern.
#[derive(Debug, Clone, PartialEq)]
pub enum Pattern {
    Triple(Node, Node, Node),
    Compare(Node, Comparison, Node),
    And(Vec<Pattern>),
    Or(Vec<Pattern>),
    Not(Box<Pattern>),
}

/// A parsed statement.
#[derive(Debug, Clone, PartialEq)]
pub enum Statement {
    Select {
        distinct: bool,
        /// The variables, or `None` for all.
        vars: Option<Vec<String>>,
        by: Vec<String>,
        pattern: Pattern,
        limit: Option<usize>,
    },
    Verify(Pattern),
    Lookup(Pattern),
    Insert {
        template: Vec<(Node, Node, Node)>,
        pattern: Option<Pattern>,
    },
    Delete {
        template: Vec<(Node, Node, Node)>,
        pattern: Option<Pattern>,
    },
}

struct Parser {
    tokens: Vec<Spanned>,
    pos: usize,
    blanks: usize,
}

impl Parser {
    fn peek(&self) -> &Token {
        &self.tokens[self.pos].token
    }

    fn here(&self) -> (usize, usize) {
        let t = &self.tokens[self.pos];
        (t.line, t.column)
    }

    fn next(&mut self) -> Token {
        let t = self.tokens[self.pos].token.clone();
        if t != Token::End {
            self.pos += 1;
        }
        t
    }

    fn is(&self, t: &Token) -> bool {
        self.peek() == t
    }

    fn eat(&mut self, t: &Token) -> bool {
        let found = self.is(t);
        if found {
            self.next();
        }
        found
    }

    fn syntax<T>(&self, message: impl Into<String>) -> Result<T> {
        let (line, column) = self.here();
        Err(Error::Syntax {
            line,
            column,
            message: message.into(),
        })
    }

    fn unsupported<T>(&self, what: impl Into<String>) -> Result<T> {
        let (line, column) = self.here();
        Err(Error::Unsupported {
            line,
            column,
            what: what.into(),
        })
    }

    fn expect(&mut self, t: &Token) -> Result<()> {
        if self.eat(t) {
            Ok(())
        } else {
            self.syntax(format!("expected {t}, found {}", self.peek()))
        }
    }

    fn keyword(&mut self, k: &'static str) -> bool {
        self.eat(&Token::Keyword(k))
    }

    fn punct(&mut self, p: &'static str) -> bool {
        self.eat(&Token::Punct(p))
    }

    fn statement(&mut self) -> Result<Statement> {
        let (line, column) = self.here();
        match self.next() {
            Token::Keyword("select") => self.select(),
            Token::Keyword("verify") => Ok(Statement::Verify(self.pattern_in()?)),
            Token::Keyword("lookup") => Ok(Statement::Lookup(self.pattern_in()?)),
            Token::Keyword(k @ ("insert" | "delete")) => {
                let template = self.template()?;
                if self.is(&Token::Keyword("into")) || self.is(&Token::Keyword("from")) {
                    return self.unsupported("a named graph");
                }
                let pattern = if self.keyword("where") {
                    Some(self.pattern()?)
                } else {
                    None
                };
                Ok(if k == "insert" {
                    Statement::Insert { template, pattern }
                } else {
                    Statement::Delete { template, pattern }
                })
            }
            Token::Keyword(k) => Err(Error::Unsupported {
                line,
                column,
                what: format!("'{k}'"),
            }),
            t => Err(Error::Syntax {
                line,
                column,
                message: format!("expected a statement, found {t}"),
            }),
        }
    }

    /// A pattern, then `in graph`, which is not supported.
    fn pattern_in(&mut self) -> Result<Pattern> {
        let p = self.pattern()?;
        if self.is(&Token::Keyword("in")) {
            return self.unsupported("a named graph");
        }
        Ok(p)
    }

    fn select(&mut self) -> Result<Statement> {
        let distinct = self.keyword("distinct");
        let vars = if self.punct("*") {
            None
        } else {
            let mut vars = vec![self.var()?];
            while self.punct(",") {
                vars.push(self.var()?);
            }
            Some(vars)
        };
        if self.is(&Token::Keyword("as")) {
            return self.unsupported("'as'");
        }
        let mut by = Vec::new();
        if self.keyword("by") {
            by.push(self.var()?);
            while self.punct(",") {
                by.push(self.var()?);
            }
        }
        let limit = if self.keyword("limit") {
            match self.next() {
                Token::Number(n) => match n.parse() {
                    Ok(n) => Some(n),
                    Err(_) => return self.syntax("expected a limit"),
                },
                _ => return self.syntax("expected a limit"),
            }
        } else {
            None
        };
        if self.is(&Token::Keyword("from")) {
            return self.unsupported("a named graph");
        }
        if !self.keyword("where") {
            return self.syntax(format!("expected 'where', found {}", self.peek()));
        }
        let pattern = self.pattern()?;
        Ok(Statement::Select {
            distinct,
            vars,
            by,
            pattern,
            limit,
        })
    }

    fn var(&mut self) -> Result<String> {
        match self.next() {
            Token::Var(v) if !v.is_empty() => Ok(v),
            Token::Keyword("distinct") => self.unsupported("'distinct' on one variable"),
            t => self.syntax(format!("expected a variable, found {t}")),
        }
    }

    /// `pattern: [let ...] exclusion`, with `else` and `implies` unsupported.
    fn pattern(&mut self) -> Result<Pattern> {
        if matches!(self.peek(), Token::Punct("+" | "-" | "*" | "/")) {
            return self.unsupported("default modifiers");
        }
        if self.is(&Token::Keyword("let")) {
            return self.unsupported("'let'");
        }
        if self.is(&Token::Keyword("graph")) {
            return self.unsupported("a named graph");
        }
        let p = self.disjunction()?;
        for k in ["else", "implies"] {
            if self.is(&Token::Keyword(k)) {
                return self.unsupported(format!("'{k}'"));
            }
        }
        Ok(p)
    }

    fn disjunction(&mut self) -> Result<Pattern> {
        let mut ps = vec![self.conjunction()?];
        while self.keyword("or") {
            ps.push(self.conjunction()?);
        }
        Ok(if ps.len() == 1 {
            ps.pop().unwrap()
        } else {
            Pattern::Or(ps)
        })
    }

    fn conjunction(&mut self) -> Result<Pattern> {
        let mut ps = vec![self.basic()?];
        while self.keyword("and") {
            ps.push(self.basic()?);
        }
        Ok(if ps.len() == 1 {
            ps.pop().unwrap()
        } else {
            Pattern::And(ps)
        })
    }

    /// `basic: subjects | ( pattern )`, and `not basic`.
    fn basic(&mut self) -> Result<Pattern> {
        if self.punct("(") {
            let p = self.pattern()?;
            self.expect(&Token::Punct(")"))?;
            return Ok(p);
        }
        if self.is(&Token::Keyword("not")) && !self.subject_follows_not() {
            self.next();
            return Ok(Pattern::Not(Box::new(self.basic()?)));
        }
        Ok(Pattern::And(self.subjects()?))
    }

    /// Whether `not` starts a negated subject (`not s p o`) rather than a
    /// negated pattern (`not ( ... )`).
    fn subject_follows_not(&self) -> bool {
        !matches!(
            self.tokens.get(self.pos + 1).map(|t| &t.token),
            Some(Token::Punct("(")) | Some(Token::Keyword("not"))
        )
    }

    /// Subjects separated by `.`, within optional braces.
    fn subjects(&mut self) -> Result<Vec<Pattern>> {
        if self.punct("{") {
            let ps = self.subjects()?;
            self.expect(&Token::Punct("}"))?;
            return Ok(ps);
        }
        let mut ps = Vec::new();
        loop {
            ps.push(self.subject()?);
            if !self.punct(".") || !self.starts_subject() {
                return Ok(ps);
            }
        }
    }

    fn starts_subject(&self) -> bool {
        matches!(
            self.peek(),
            Token::Var(_)
                | Token::Label(_)
                | Token::Key(_)
                | Token::Ident(_)
                | Token::Text(_)
                | Token::Number(_)
                | Token::Punct("[")
                | Token::Keyword("not" | "true" | "false" | "list")
        )
    }

    /// `subject: not node predicates | node predicate_phrase`; calls are
    /// not supported.
    fn subject(&mut self) -> Result<Pattern> {
        let negated = self.keyword("not");
        let mut out = Vec::new();
        let node = self.node(&mut out)?;
        if self.is(&Token::Punct("(")) {
            return self.unsupported("a call");
        }
        let cmp = match self.peek() {
            Token::Punct("=") | Token::Punct("==") => Some(Comparison::Eq),
            Token::Punct("!=") => Some(Comparison::Ne),
            Token::Punct("<") => Some(Comparison::Data(Cmp::Lt)),
            Token::Punct("<=") => Some(Comparison::Data(Cmp::Le)),
            Token::Punct(">") => Some(Comparison::Data(Cmp::Gt)),
            Token::Punct(">=") => Some(Comparison::Data(Cmp::Ge)),
            _ => None,
        };
        if let Some(cmp) = cmp.filter(|_| !negated) {
            self.next();
            let other = self.node(&mut out)?;
            out.push(Pattern::Compare(node, cmp, other));
        } else {
            self.predicates(&node, &mut out)?;
        }
        let p = Pattern::And(out);
        Ok(if negated {
            Pattern::Not(Box::new(p))
        } else {
            p
        })
    }

    /// Predicates of `subject`, with their objects, separated by `;`.
    fn predicates(&mut self, subject: &Node, out: &mut Vec<Pattern>) -> Result<()> {
        loop {
            match self.peek() {
                Token::Punct("<<" | ">>") => return self.unsupported("'<<' and '>>'"),
                Token::Punct("+" | "-" | "*" | "/") | Token::Keyword("limit" | "offset") => {
                    return self.unsupported("predicate modifiers");
                }
                _ => {}
            }
            let negated = self.keyword("not") || self.punct("!");
            let predicate = match self.peek() {
                Token::Ident(a) if a == "a" => {
                    self.next();
                    Node::Value("type".into())
                }
                _ => self.node(out)?,
            };
            if self.is(&Token::Punct("++")) {
                return self.unsupported("transitive predicates");
            }
            loop {
                let object_negated = self.keyword("not");
                let object = self.object(out)?;
                if self.starts_node() {
                    return self.unsupported("author and subgraph positions");
                }
                let triple = Pattern::Triple(subject.clone(), predicate.clone(), object);
                out.push(if negated || object_negated {
                    Pattern::Not(Box::new(triple))
                } else {
                    triple
                });
                if !self.punct(",") {
                    break;
                }
            }
            match self.peek() {
                Token::Punct("+" | "-" | "*" | "/" | "!") | Token::Keyword("limit" | "offset") => {
                    return self.unsupported("predicate modifiers");
                }
                _ => {}
            }
            if !self.punct(";") {
                return Ok(());
            }
            if !self.starts_node() && !self.is(&Token::Keyword("not")) {
                // A trailing `;`.
                return Ok(());
            }
        }
    }

    fn object(&mut self, out: &mut Vec<Pattern>) -> Result<Node> {
        if self.is(&Token::Punct("(")) {
            return self.unsupported("an expression object");
        }
        self.node(out)
    }

    fn starts_node(&self) -> bool {
        matches!(
            self.peek(),
            Token::Var(_)
                | Token::Label(_)
                | Token::Key(_)
                | Token::Ident(_)
                | Token::Text(_)
                | Token::Number(_)
                | Token::Punct("[")
                | Token::Keyword("true" | "false" | "list")
        )
    }

    fn fresh(&mut self) -> String {
        self.blanks += 1;
        format!("[{}]", self.blanks)
    }

    /// A node; a blank node's predicates are added to `out`.
    fn node(&mut self, out: &mut Vec<Pattern>) -> Result<Node> {
        let (line, column) = self.here();
        match self.next() {
            Token::Var(v) if v.is_empty() => Ok(Node::Blank(self.fresh())),
            Token::Var(v) => Ok(Node::Var(v)),
            Token::Label(l) => Ok(Node::Blank(format!("_:{l}"))),
            Token::Key(k) => Ok(Node::Key(k)),
            Token::Ident(i) | Token::Text(i) | Token::Number(i) => Ok(Node::Value(i)),
            Token::Keyword(k @ ("true" | "false")) => Ok(Node::Value(k.into())),
            Token::Keyword("list") => Err(Error::Unsupported {
                line,
                column,
                what: "lists".into(),
            }),
            Token::Punct("[") => {
                let node = if self.punct(":") {
                    match self.next() {
                        Token::Var(v) if !v.is_empty() => Node::Var(v),
                        t => return self.syntax(format!("expected a variable, found {t}")),
                    }
                } else {
                    Node::Blank(self.fresh())
                };
                if !self.is(&Token::Punct("]")) {
                    self.predicates(&node, out)?;
                }
                self.expect(&Token::Punct("]"))?;
                Ok(node)
            }
            t => Err(Error::Syntax {
                line,
                column,
                message: format!("expected a node, found {t}"),
            }),
        }
    }

    /// `template: subjects`, of triples only.
    fn template(&mut self) -> Result<Vec<(Node, Node, Node)>> {
        if self.is(&Token::Keyword("graph")) || self.is(&Token::Punct("(")) {
            return self.unsupported("a graph template");
        }
        let (line, column) = self.here();
        let mut triples = Vec::new();
        for p in self.subjects()? {
            flatten(p, &mut triples).map_err(|what| Error::Unsupported {
                line,
                column,
                what: what.into(),
            })?;
        }
        Ok(triples)
    }
}

/// The triples of a template pattern.
fn flatten(p: Pattern, out: &mut Vec<(Node, Node, Node)>) -> std::result::Result<(), &'static str> {
    match p {
        Pattern::Triple(s, p, o) => out.push((s, p, o)),
        Pattern::And(ps) => {
            for p in ps {
                flatten(p, out)?;
            }
        }
        _ => return Err("a template other than triples"),
    }
    Ok(())
}

/// Parses GraQL statements.
pub fn parse(text: &str) -> Result<Vec<Statement>> {
    let mut parser = Parser {
        tokens: lex(text)?,
        pos: 0,
        blanks: 0,
    };
    let mut statements = Vec::new();
    while !parser.is(&Token::End) {
        if parser.keyword("quit") {
            break;
        }
        statements.push(parser.statement()?);
    }
    Ok(statements)
}

/// Builds queries from parsed patterns, against a graph.
struct Lowering<'g> {
    g: &'g Graph,
    query: Query,
    vars: HashMap<String, Var>,
}

impl Lowering<'_> {
    fn var(&mut self, name: &str) -> Var {
        if let Some(&v) = self.vars.get(name) {
            return v;
        }
        let v = self.query.var(name);
        self.vars.insert(name.to_string(), v);
        v
    }

    /// The term for `node`; a constant missing from the graph is 0, which
    /// matches nothing.
    fn term(&mut self, node: &Node) -> Term {
        match node {
            Node::Var(v) | Node::Blank(v) => Term::Var(self.var(v)),
            Node::Key(k) => Term::Id(self.g.find_by_key(k.as_bytes()).unwrap_or(0)),
            Node::Value(v) => Term::Id(self.g.find_value(v.as_bytes()).unwrap_or(0)),
        }
    }

    fn group(&mut self, p: &Pattern) -> Group {
        let mut group = Group::new();
        self.add(&mut group, p);
        group
    }

    fn add(&mut self, group: &mut Group, p: &Pattern) {
        match p {
            Pattern::Triple(s, pr, o) => {
                let (s, pr, o) = (self.term(s), self.term(pr), self.term(o));
                group.pattern(s, pr, o);
            }
            Pattern::Compare(a, cmp, b) => {
                let (ta, tb) = (self.term(a), self.term(b));
                let filter = match (*cmp, ta, b, tb, a) {
                    (Comparison::Eq, ..) => Filter::Eq(ta, tb),
                    (Comparison::Ne, ..) => Filter::Ne(ta, tb),
                    // A variable and a value's data, which need not be in
                    // the graph.
                    (Comparison::Data(c), Term::Var(v), Node::Value(data), ..)
                    | (Comparison::Data(c), _, _, Term::Var(v), Node::Value(data)) => {
                        let c = if matches!(ta, Term::Var(_)) {
                            c
                        } else {
                            flip(c)
                        };
                        let data = data.clone().into_bytes();
                        Filter::Test(
                            v,
                            Arc::new(move |g: &Graph, id| {
                                g.data(id).is_some_and(|d| c.holds(compare_data(d, &data)))
                            }),
                        )
                    }
                    (Comparison::Data(c), ..) => Filter::Compare(ta, c, tb),
                };
                group.filter(filter);
            }
            Pattern::And(ps) => {
                for p in ps {
                    self.add(group, p);
                }
            }
            Pattern::Or(ps) => {
                let branches: Vec<Group> = ps.iter().map(|p| self.group(p)).collect();
                group.union(branches);
            }
            Pattern::Not(p) => {
                let not = self.group(p);
                group.not(not);
            }
        }
    }
}

/// The comparison with its sides swapped.
fn flip(c: Cmp) -> Cmp {
    match c {
        Cmp::Lt => Cmp::Gt,
        Cmp::Le => Cmp::Ge,
        Cmp::Gt => Cmp::Lt,
        Cmp::Ge => Cmp::Le,
    }
}

/// The query of a pattern, and its variables by name.
fn lower(g: &Graph, p: &Pattern) -> (Query, HashMap<String, Var>) {
    let mut l = Lowering {
        g,
        query: Query::new(),
        vars: HashMap::new(),
    };
    let root = l.group(p);
    *l.query.group() = root;
    (l.query, l.vars)
}

/// The names of the variables (and blank nodes, if `blanks`) every row of
/// a pattern binds, in order of appearance.
fn bound_names(p: &Pattern, blanks: bool) -> Vec<String> {
    fn collect(p: &Pattern, blanks: bool, out: &mut Vec<String>) {
        match p {
            Pattern::Triple(s, pr, o) => {
                for n in [s, pr, o] {
                    match n {
                        Node::Var(name) => out.push(name.clone()),
                        Node::Blank(name) if blanks => out.push(name.clone()),
                        _ => {}
                    }
                }
            }
            Pattern::And(ps) => ps.iter().for_each(|p| collect(p, blanks, out)),
            // Every branch binds the same variables.
            Pattern::Or(ps) => {
                if let Some(p) = ps.first() {
                    collect(p, blanks, out);
                }
            }
            Pattern::Compare(..) | Pattern::Not(_) => {}
        }
    }
    let mut names = Vec::new();
    collect(p, blanks, &mut names);
    let mut seen = BTreeSet::new();
    names.retain(|n| seen.insert(n.clone()));
    names
}

/// The positive triples of a pattern, as instantiated by its rows.
fn triples(p: &Pattern, out: &mut Vec<(Node, Node, Node)>) {
    match p {
        Pattern::Triple(s, pr, o) => out.push((s.clone(), pr.clone(), o.clone())),
        Pattern::And(ps) | Pattern::Or(ps) => ps.iter().for_each(|p| triples(p, out)),
        Pattern::Compare(..) | Pattern::Not(_) => {}
    }
}

impl Statement {
    /// The query of a `select`, `verify` or `lookup` statement against `g`,
    /// and the names of the variables its rows return.
    pub fn query(&self, g: &Graph) -> Result<(Query, Vec<String>)> {
        let unsupported = |what: &str| Error::Unsupported {
            line: 0,
            column: 0,
            what: what.into(),
        };
        match self {
            Statement::Select {
                distinct,
                vars,
                by,
                pattern,
                limit,
            } => {
                let (mut q, map) = lower(g, pattern);
                let names = match vars {
                    Some(vars) => vars.clone(),
                    None => bound_names(pattern, false),
                };
                let lookup = |name: &String| {
                    map.get(name).copied().ok_or_else(|| Error::Syntax {
                        line: 0,
                        column: 0,
                        message: format!("?{name} is not in the pattern"),
                    })
                };
                let selected: Vec<Var> = names.iter().map(lookup).collect::<Result<_>>()?;
                q.select(selected);
                if *distinct {
                    q.distinct();
                }
                for name in by {
                    q.order_by(SortKey::data(lookup(name)?));
                }
                if let Some(n) = limit {
                    q.limit(*n);
                }
                Ok((q, names))
            }
            Statement::Verify(pattern) => {
                let (mut q, _) = lower(g, pattern);
                q.select([]).limit(1);
                Ok((q, Vec::new()))
            }
            Statement::Lookup(pattern) => {
                let (mut q, map) = lower(g, pattern);
                let names = bound_names(pattern, true);
                q.select(names.iter().map(|n| map[n]));
                Ok((q, names))
            }
            _ => Err(unsupported("a query of an insert or delete")),
        }
    }
}

/// The result of a statement.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Output {
    /// The rows of a `select`, with the names of their variables.
    Rows {
        vars: Vec<String>,
        rows: Vec<Vec<u64>>,
    },
    /// Whether a `verify`'s pattern has a match.
    Verified(bool),
    /// The links of a `lookup`'s matches.
    Links(Vec<u64>),
    /// The links an `insert` added.
    Inserted(usize),
    /// The links a `delete` deleted.
    Deleted(usize),
}

/// The values of a pattern's variables in each of its rows (or a single row
/// binding nothing, with no pattern).
fn bindings(g: &Graph, pattern: Option<&Pattern>) -> Result<Vec<HashMap<String, u64>>> {
    let Some(pattern) = pattern else {
        return Ok(vec![HashMap::new()]);
    };
    let (mut q, map) = lower(g, pattern);
    let names = bound_names(pattern, true);
    q.select(names.iter().map(|n| map[n]));
    let rows = q.run(g)?;
    Ok(rows
        .into_iter()
        .map(|row| names.iter().cloned().zip(row).collect())
        .collect())
}

/// Runs a parsed statement on `g`.
pub fn run(g: &mut Graph, statement: &Statement) -> Result<Output> {
    match statement {
        Statement::Select { .. } => {
            let (q, vars) = statement.query(g)?;
            Ok(Output::Rows {
                vars,
                rows: q.run(g)?,
            })
        }
        Statement::Verify(_) => {
            let (q, _) = statement.query(g)?;
            Ok(Output::Verified(!q.run(g)?.is_empty()))
        }
        Statement::Lookup(pattern) => {
            let mut links = BTreeSet::new();
            let mut positive = Vec::new();
            triples(pattern, &mut positive);
            for row in bindings(g, Some(pattern))? {
                for (s, p, o) in &positive {
                    let id = |n: &Node| match n {
                        Node::Var(v) | Node::Blank(v) => row.get(v).copied(),
                        Node::Key(k) => g.find_by_key(k.as_bytes()),
                        Node::Value(v) => g.find_value(v.as_bytes()),
                    };
                    if let (Some(s), Some(p), Some(o)) = (id(s), id(p), id(o))
                        && let Some(l) = g.find_link(s, p, o)
                    {
                        links.insert(l);
                    }
                }
            }
            Ok(Output::Links(links.into_iter().collect()))
        }
        Statement::Insert { template, pattern } | Statement::Delete { template, pattern } => {
            let insert = matches!(statement, Statement::Insert { .. });
            let bound = pattern
                .as_ref()
                .map(|p| bound_names(p, true))
                .unwrap_or_default();
            let unbound = template.iter().any(|(s, p, o)| {
                [s, p, o]
                    .iter()
                    .any(|n| matches!(n, Node::Var(v) if !bound.contains(v)))
            });
            if unbound {
                return Err(Error::Syntax {
                    line: 0,
                    column: 0,
                    message: "a template variable is not bound by the pattern".into(),
                });
            }
            let mut count = 0;
            for row in bindings(g, pattern.as_ref())? {
                // A blank node of the template is a new node for each row.
                let mut blanks: HashMap<String, u64> = HashMap::new();
                for (s, p, o) in template {
                    let mut id = |g: &mut Graph, n: &Node| -> graph::Result<Option<u64>> {
                        Ok(match n {
                            Node::Var(v) | Node::Blank(v) if row.contains_key(v) => Some(row[v]),
                            Node::Var(_) => None,
                            Node::Blank(b) if insert => Some(match blanks.get(b) {
                                Some(&id) => id,
                                None => {
                                    let id = g.node()?;
                                    blanks.insert(b.clone(), id);
                                    id
                                }
                            }),
                            Node::Blank(_) => None,
                            Node::Key(k) if insert => Some(g.node_by_key(k.as_bytes())?),
                            Node::Key(k) => g.find_by_key(k.as_bytes()),
                            Node::Value(v) if insert => Some(g.value(v.as_bytes())?),
                            Node::Value(v) => g.find_value(v.as_bytes()),
                        })
                    };
                    // Deleting a link to a constant not in the graph does nothing.
                    let (Some(s), Some(p), Some(o)) = (id(g, s)?, id(g, p)?, id(g, o)?) else {
                        continue;
                    };
                    if insert {
                        if g.find_link(s, p, o).is_none() {
                            g.link(s, p, o)?;
                            count += 1;
                        }
                    } else if g.delete_link(s, p, o)?.is_some() {
                        count += 1;
                    }
                }
            }
            Ok(if insert {
                Output::Inserted(count)
            } else {
                Output::Deleted(count)
            })
        }
    }
}

/// Parses and runs GraQL statements on `g`, returning each one's output.
pub fn execute(g: &mut Graph, text: &str) -> Result<Vec<Output>> {
    parse(text)?.iter().map(|s| run(g, s)).collect()
}

#[cfg(test)]
mod tests;
