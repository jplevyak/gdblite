//! Graph pattern queries, planned by cost and evaluated in batches.
//!
//! A [`Query`] is a set of triple patterns over variables, with filters, a
//! selection of variables to return, a limit, and the id as of which to see
//! the graph.  Each pattern matches the live links of the graph; a link
//! pattern also binds the link's own id, so queries can match statements
//! about links.
//!
//! ```
//! use gdblite::graph::{Graph, Options};
//! use gdblite::query::{Filter, Query};
//!
//! let mut g = Graph::open(None, Options::default()).unwrap();
//! let knows = g.value(b"knows").unwrap();
//! let (a, b, c) = (g.node().unwrap(), g.node().unwrap(), g.node().unwrap());
//! g.link(a, knows, b).unwrap();
//! g.link(b, knows, c).unwrap();
//! g.link(c, knows, a).unwrap();
//!
//! // Who do the people a knows know?
//! let mut q = Query::new();
//! let (x, y) = (q.var("x"), q.var("y"));
//! q.pattern(a, knows, x).pattern(x, knows, y).select([y]);
//! assert_eq!(q.run(&g).unwrap(), [[c]]);
//!
//! // Pairs who know each other in a triangle, without listing it three times.
//! let mut q = Query::new();
//! let (x, y, z) = (q.var("x"), q.var("y"), q.var("z"));
//! q.pattern(x, knows, y)
//!     .pattern(y, knows, z)
//!     .pattern(z, knows, x)
//!     .filter(Filter::Lt(x.into(), y.into()))
//!     .filter(Filter::Lt(x.into(), z.into()));
//! let plan = q.plan(&g).unwrap();
//! println!("{}", plan.explain());
//! assert_eq!(plan.execute(&g).count(), 1);
//! ```
//!
//! # Planning
//!
//! [`Query::plan`] orders the patterns to minimize an estimated cost, using
//! the counts of the indexes for constants and the graph's
//! [`Stats`](crate::stats::Stats) for variables, and chooses for each
//! pattern after the first how to join it:
//!
//! - a *bind join* looks the pattern up in the best index for each row so
//!   far, given the values bound; good when the rows so far are few.
//! - a *hash join* scans the pattern once, by its constants alone, and joins
//!   it to the rows so far on their shared variables; good when the pattern
//!   alone matches few links.
//!
//! Filters are applied as soon as their variables are bound.  Up to
//! [`EXHAUSTIVE_PATTERNS`] patterns are planned exhaustively over left-deep
//! orders, more greedily.
//!
//! # Results
//!
//! [`Plan::execute`] returns the rows as they are produced, so a limit or a
//! caller that stops early stops the work.  Rows are not deduplicated: since
//! a graph has one live link per subject, predicate and object, rows binding
//! every variable are distinct, but selecting fewer variables can repeat
//! rows.

use std::fmt;
use std::sync::Arc;

use crate::graph::{Error, Graph, Result};

mod exec;
mod plan;
#[cfg(test)]
mod tests;

pub use exec::Rows;
pub use plan::{EXHAUSTIVE_PATTERNS, Plan};

/// A query variable, made by [`Query::var`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Var(usize);

impl Var {
    /// The variable's number in its query, from 0.
    pub fn index(self) -> usize {
        self.0
    }
}

/// A variable or an id.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Term {
    Var(Var),
    Id(u64),
}

impl From<Var> for Term {
    fn from(v: Var) -> Self {
        Term::Var(v)
    }
}

impl From<u64> for Term {
    fn from(id: u64) -> Self {
        Term::Id(id)
    }
}

impl Term {
    fn var(self) -> Option<Var> {
        match self {
            Term::Var(v) => Some(v),
            Term::Id(_) => None,
        }
    }
}

/// A pattern matching the live links with its subject, predicate and object,
/// binding `link`, if given, to the link's id.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Pattern {
    pub link: Option<Var>,
    pub subject: Term,
    pub predicate: Term,
    pub object: Term,
}

impl Pattern {
    fn terms(&self) -> [Term; 3] {
        [self.subject, self.predicate, self.object]
    }

    /// The pattern's variables, possibly repeated.
    fn vars(&self) -> impl Iterator<Item = Var> + '_ {
        self.terms()
            .into_iter()
            .filter_map(Term::var)
            .chain(self.link)
    }
}

/// A test of one bound id.
pub type Test = Arc<dyn Fn(&Graph, u64) -> bool + Send + Sync>;

/// A condition on the rows of a query.
#[derive(Clone)]
pub enum Filter {
    Eq(Term, Term),
    Ne(Term, Term),
    /// The first id is less than the second: useful for breaking symmetry.
    Lt(Term, Term),
    /// `test(graph, id)` holds for the id bound to the variable, for example
    /// a test of the data of a value.
    Test(Var, Test),
}

impl fmt::Debug for Filter {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Filter::Eq(a, b) => write!(f, "Eq({a:?}, {b:?})"),
            Filter::Ne(a, b) => write!(f, "Ne({a:?}, {b:?})"),
            Filter::Lt(a, b) => write!(f, "Lt({a:?}, {b:?})"),
            Filter::Test(v, _) => write!(f, "Test({v:?})"),
        }
    }
}

impl Filter {
    fn vars(&self) -> Vec<Var> {
        match self {
            Filter::Eq(a, b) | Filter::Ne(a, b) | Filter::Lt(a, b) => {
                [a, b].into_iter().filter_map(|t| t.var()).collect()
            }
            Filter::Test(v, _) => vec![*v],
        }
    }

    /// Whether `row`, which binds the filter's variables, passes.
    fn eval(&self, g: &Graph, row: &[u64]) -> bool {
        let id = |t: &Term| match *t {
            Term::Var(v) => row[v.0],
            Term::Id(id) => id,
        };
        match self {
            Filter::Eq(a, b) => id(a) == id(b),
            Filter::Ne(a, b) => id(a) != id(b),
            Filter::Lt(a, b) => id(a) < id(b),
            Filter::Test(v, test) => test(g, row[v.0]),
        }
    }

    /// The estimated fraction of rows that pass.
    fn selectivity(&self) -> f64 {
        match self {
            Filter::Eq(..) => 0.1,
            Filter::Ne(..) => 0.9,
            Filter::Lt(..) => 0.5,
            Filter::Test(..) => 0.5,
        }
    }
}

/// A graph pattern query; see the [module](self) docs.
#[derive(Debug, Clone, Default)]
pub struct Query {
    names: Vec<String>,
    patterns: Vec<Pattern>,
    filters: Vec<Filter>,
    select: Option<Vec<Var>>,
    limit: Option<usize>,
    as_of: Option<u64>,
}

impl Query {
    pub fn new() -> Self {
        Self::default()
    }

    /// A new variable; `name` is for [`Plan::explain`].
    pub fn var(&mut self, name: &str) -> Var {
        self.names.push(name.to_string());
        Var(self.names.len() - 1)
    }

    /// Adds a pattern matching live links with this subject, predicate and
    /// object.
    pub fn pattern(
        &mut self,
        subject: impl Into<Term>,
        predicate: impl Into<Term>,
        object: impl Into<Term>,
    ) -> &mut Self {
        self.patterns.push(Pattern {
            link: None,
            subject: subject.into(),
            predicate: predicate.into(),
            object: object.into(),
        });
        self
    }

    /// Adds a pattern like [`pattern`](Self::pattern) that also binds `link`
    /// to the id of the link matched.
    pub fn link_pattern(
        &mut self,
        link: Var,
        subject: impl Into<Term>,
        predicate: impl Into<Term>,
        object: impl Into<Term>,
    ) -> &mut Self {
        self.pattern(subject, predicate, object);
        self.patterns.last_mut().unwrap().link = Some(link);
        self
    }

    pub fn filter(&mut self, filter: Filter) -> &mut Self {
        self.filters.push(filter);
        self
    }

    /// The variables each row returns, in order; by default, all of them.
    pub fn select(&mut self, vars: impl IntoIterator<Item = Var>) -> &mut Self {
        self.select = Some(vars.into_iter().collect());
        self
    }

    /// Returns at most `n` rows.
    pub fn limit(&mut self, n: usize) -> &mut Self {
        self.limit = Some(n);
        self
    }

    /// Sees the graph as it was when `id` was appended.
    pub fn as_of(&mut self, id: u64) -> &mut Self {
        self.as_of = Some(id);
        self
    }

    pub fn patterns(&self) -> &[Pattern] {
        &self.patterns
    }

    /// Plans the query against `g`'s indexes and statistics.  Fails if there
    /// are no patterns, or a variable used is not in any pattern.
    pub fn plan(&self, g: &Graph) -> Result<Plan> {
        plan::plan(self, g)
    }

    /// Plans and runs the query, collecting its rows.
    pub fn run(&self, g: &Graph) -> Result<Vec<Vec<u64>>> {
        self.plan(g)?.execute(g).collect()
    }

    /// The selected variables.
    fn selected(&self) -> Vec<Var> {
        self.select
            .clone()
            .unwrap_or_else(|| (0..self.names.len()).map(Var).collect())
    }

    /// Checks that every variable used is in a pattern.
    fn check(&self) -> Result<()> {
        if self.patterns.is_empty() {
            return Err(Error::InvalidArgument("a query needs a pattern"));
        }
        let mut bound = vec![false; self.names.len()];
        for v in self.patterns.iter().flat_map(Pattern::vars) {
            bound[v.0] = true;
        }
        let used = self
            .filters
            .iter()
            .flat_map(Filter::vars)
            .chain(self.selected());
        for v in used {
            if !bound.get(v.0).copied().unwrap_or(false) {
                return Err(Error::InvalidArgument(
                    "a variable is not bound by any pattern",
                ));
            }
        }
        Ok(())
    }

    fn term_name(&self, t: Term) -> String {
        match t {
            Term::Var(v) => format!("?{}", self.names[v.0]),
            Term::Id(id) => id.to_string(),
        }
    }

    fn pattern_name(&self, p: &Pattern) -> String {
        let spo = format!(
            "{} {} {}",
            self.term_name(p.subject),
            self.term_name(p.predicate),
            self.term_name(p.object)
        );
        match p.link {
            Some(l) => format!("?{} = ({spo})", self.names[l.0]),
            None => format!("({spo})"),
        }
    }

    fn filter_name(&self, f: &Filter) -> String {
        match f {
            Filter::Eq(a, b) => format!("{} = {}", self.term_name(*a), self.term_name(*b)),
            Filter::Ne(a, b) => format!("{} != {}", self.term_name(*a), self.term_name(*b)),
            Filter::Lt(a, b) => format!("{} < {}", self.term_name(*a), self.term_name(*b)),
            Filter::Test(v, _) => format!("test(?{})", self.names[v.0]),
        }
    }
}
