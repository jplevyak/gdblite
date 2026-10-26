//! Graph pattern queries, planned by cost and evaluated in batches.
//!
//! A [`Query`] is a [`Group`] of triple patterns over variables, unions of
//! groups, negated groups and filters, with a selection of variables to
//! return, deduplication, ordering, a limit, and the id as of which to see
//! the graph.  Each pattern matches the live links of the graph; a link
//! pattern also binds the link's own id, so queries can match statements
//! about links.
//!
//! ```
//! use gdblite::graph::{Graph, Options};
//! use gdblite::query::{Filter, Group, Query, SortKey};
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
//! // Who knows someone a does not know, other than a?
//! let mut q = Query::new();
//! let (x, y) = (q.var("x"), q.var("y"));
//! let mut known_by_a = Group::new();
//! known_by_a.pattern(a, knows, y);
//! q.pattern(x, knows, y)
//!     .not(known_by_a)
//!     .filter(Filter::Ne(x.into(), a.into()))
//!     .select([x])
//!     .distinct()
//!     .order_by(SortKey::id(x));
//! let plan = q.plan(&g).unwrap();
//! println!("{}", plan.explain());
//! assert_eq!(plan.execute(&g).collect::<Result<Vec<_>, _>>().unwrap(), [[b], [c]]);
//! ```
//!
//! # Groups
//!
//! A group's rows are those matching all of its patterns and unions, that
//! pass its filters, and for which none of its negated groups has a match.
//!
//! - A *union* matches the rows of any of its branch groups.  Every branch
//!   must bind the same variables, so that every row binds every variable.
//! - A *negated group* (`not`) removes the rows for which it has a match,
//!   given the values the row binds to its variables: SPARQL's `FILTER NOT
//!   EXISTS`.  Its other variables are its own.
//! - A *filter* may use the variables of its group and of the groups it is
//!   in.
//!
//! # Planning
//!
//! [`Query::plan`] first *reduces* the top group's patterns: by constraint
//! propagation, as the C++ GraQL evaluator did, it narrows each variable to
//! a set of candidate ids and each pattern to its candidate links, starting
//! from the constants and the most selective patterns, until nothing changes
//! or a budget of index entries read
//! ([`reduction_budget`](Query::reduction_budget)) is spent.  The joins then
//! read the reduced links rather than the indexes, and their estimates are
//! exact.  A plan only uses its reduction while the graph is as it was when
//! planned.
//!
//! It then orders the patterns and unions of each group to minimize
//! an estimated cost, using the counts of the indexes for constants and the
//! graph's [`Stats`](crate::stats::Stats) for variables, and chooses for
//! each how to join it:
//!
//! - a *bind join* evaluates it for each row so far, given the values bound:
//!   for a pattern, a lookup in the best index; good when the rows so far
//!   are few.
//! - a *hash join* evaluates it once, by its constants alone, and joins it
//!   to the rows so far on their shared variables; good when it alone
//!   matches few links.
//!
//! Negated groups are chosen between the same two ways.  Filters and
//! negations are applied as soon as the variables they share with their
//! group are bound.  Up to [`EXHAUSTIVE_PATTERNS`] patterns and unions of a
//! group are planned exhaustively over left-deep orders, more greedily.
//!
//! # Standing queries
//!
//! A [`StandingQuery`] keeps a query's rows up to date as entries are
//! committed: each [`update`](StandingQuery::update) works out the
//! [`Changes`] from the links committed since, without evaluating the query
//! again.
//!
//! # Results
//!
//! [`Plan::execute`] returns the rows as they are produced, so a limit or a
//! caller that stops early stops the work, unless the rows are ordered.
//! Rows are deduplicated only if [`distinct`](Query::distinct) is set: since
//! a graph has one live link per subject, predicate and object, rows of
//! patterns binding every variable are distinct, but selecting fewer
//! variables, or unions, can repeat rows.

use std::cmp::Ordering;
use std::collections::BTreeSet;
use std::fmt;
use std::sync::Arc;

use crate::graph::{Error, Graph, Result};

mod exec;
mod plan;
mod reduce;
mod standing;
#[cfg(test)]
mod tests;

pub use exec::Rows;
pub use plan::{EXHAUSTIVE_PATTERNS, Plan};
pub use reduce::DEFAULT_REDUCTION_BUDGET;
pub use standing::{Changes, StandingQuery};

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
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Pattern {
    pub link: Option<Var>,
    pub subject: Term,
    pub predicate: Term,
    pub object: Term,
    /// The id as of which this pattern sees the graph, overriding the
    /// query's.
    pub as_of: Option<u64>,
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

/// A condition on the rows of a group.
#[derive(Clone)]
pub enum Filter {
    Eq(Term, Term),
    Ne(Term, Term),
    /// The first id is less than the second: useful for breaking symmetry.
    Lt(Term, Term),
    /// The data of the first value compares to the second's as given:
    /// numerically if both are numbers, else by bytes.  Fails for ids
    /// without data.
    Compare(Term, Cmp, Term),
    /// `test(graph, id)` holds for the id bound to the variable, for example
    /// a test of the data of a value.
    Test(Var, Test),
}

/// A comparison of the data of two values.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Cmp {
    Lt,
    Le,
    Gt,
    Ge,
}

impl Cmp {
    /// Whether `ordering` satisfies the comparison.
    pub fn holds(self, ordering: Ordering) -> bool {
        match self {
            Cmp::Lt => ordering.is_lt(),
            Cmp::Le => ordering.is_le(),
            Cmp::Gt => ordering.is_gt(),
            Cmp::Ge => ordering.is_ge(),
        }
    }

    fn symbol(self) -> &'static str {
        match self {
            Cmp::Lt => "<",
            Cmp::Le => "<=",
            Cmp::Gt => ">",
            Cmp::Ge => ">=",
        }
    }
}

/// Compares two data: numerically if both are numbers, else by bytes.
pub fn compare_data(a: &[u8], b: &[u8]) -> Ordering {
    let number = |d: &[u8]| std::str::from_utf8(d).ok()?.trim().parse::<f64>().ok();
    match (number(a), number(b)) {
        (Some(x), Some(y)) => x.total_cmp(&y),
        _ => a.cmp(b),
    }
}

impl fmt::Debug for Filter {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Filter::Eq(a, b) => write!(f, "Eq({a:?}, {b:?})"),
            Filter::Ne(a, b) => write!(f, "Ne({a:?}, {b:?})"),
            Filter::Lt(a, b) => write!(f, "Lt({a:?}, {b:?})"),
            Filter::Compare(a, c, b) => write!(f, "Compare({a:?}, {c:?}, {b:?})"),
            Filter::Test(v, _) => write!(f, "Test({v:?})"),
        }
    }
}

impl Filter {
    fn vars(&self) -> Vec<Var> {
        match self {
            Filter::Eq(a, b) | Filter::Ne(a, b) | Filter::Lt(a, b) | Filter::Compare(a, _, b) => {
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
            Filter::Compare(a, c, b) => match (g.data(id(a)), g.data(id(b))) {
                (Some(x), Some(y)) => c.holds(compare_data(x, y)),
                _ => false,
            },
            Filter::Test(v, test) => test(g, row[v.0]),
        }
    }

    /// The estimated fraction of rows that pass.
    fn selectivity(&self) -> f64 {
        match self {
            Filter::Eq(..) => 0.1,
            Filter::Ne(..) => 0.9,
            Filter::Lt(..) | Filter::Compare(..) => 0.5,
            Filter::Test(..) => 0.5,
        }
    }
}

/// Patterns, unions, negations and filters; see the [module](self) docs.
#[derive(Debug, Clone, Default)]
pub struct Group {
    patterns: Vec<Pattern>,
    unions: Vec<Vec<Group>>,
    nots: Vec<Group>,
    filters: Vec<Filter>,
}

impl Group {
    pub fn new() -> Self {
        Self::default()
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
            as_of: None,
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

    /// Makes the last pattern added see the graph as it was when `id` was
    /// appended.
    pub fn at(&mut self, id: u64) -> &mut Self {
        if let Some(p) = self.patterns.last_mut() {
            p.as_of = Some(id);
        }
        self
    }

    /// Adds a union of `branches`, which must bind the same variables.
    pub fn union(&mut self, branches: impl IntoIterator<Item = Group>) -> &mut Self {
        self.unions.push(branches.into_iter().collect());
        self
    }

    /// Removes the rows for which `group` has a match.
    pub fn not(&mut self, group: Group) -> &mut Self {
        self.nots.push(group);
        self
    }

    pub fn filter(&mut self, filter: Filter) -> &mut Self {
        self.filters.push(filter);
        self
    }

    pub fn patterns(&self) -> &[Pattern] {
        &self.patterns
    }

    /// The variables every row of the group binds.
    fn bound_vars(&self) -> BTreeSet<Var> {
        let mut vars: BTreeSet<Var> = self.patterns.iter().flat_map(Pattern::vars).collect();
        for branches in &self.unions {
            if let Some(b) = branches.first() {
                vars.extend(b.bound_vars());
            }
        }
        vars
    }

    /// Every variable the group uses.
    fn all_vars(&self) -> BTreeSet<Var> {
        let mut vars = self.bound_vars();
        vars.extend(self.filters.iter().flat_map(Filter::vars));
        for g in self.unions.iter().flatten().chain(&self.nots) {
            vars.extend(g.all_vars());
        }
        vars
    }

    /// Checks that the group and those in it have a pattern or union, and
    /// that the branches of each union bind the same variables.
    fn check(&self) -> Result<()> {
        if self.patterns.is_empty() && self.unions.is_empty() {
            return Err(Error::InvalidArgument("a group needs a pattern or a union"));
        }
        for branches in &self.unions {
            let Some(first) = branches.first() else {
                return Err(Error::InvalidArgument("a union needs a branch"));
            };
            if branches
                .iter()
                .any(|b| b.bound_vars() != first.bound_vars())
            {
                return Err(Error::InvalidArgument(
                    "the branches of a union must bind the same variables",
                ));
            }
        }
        for g in self.unions.iter().flatten().chain(&self.nots) {
            g.check()?;
        }
        Ok(())
    }
}

/// A key to order rows by.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SortKey {
    pub var: Var,
    /// Orders by the data of values (nodes first) rather than by id.
    pub by_data: bool,
    pub descending: bool,
}

impl SortKey {
    /// Orders by the id bound to `var`.
    pub fn id(var: Var) -> Self {
        SortKey {
            var,
            by_data: false,
            descending: false,
        }
    }

    /// Orders by the data of the value bound to `var`, then by id.
    pub fn data(var: Var) -> Self {
        SortKey {
            by_data: true,
            ..Self::id(var)
        }
    }

    pub fn desc(self) -> Self {
        SortKey {
            descending: true,
            ..self
        }
    }
}

/// A graph pattern query; see the [module](self) docs.
#[derive(Debug, Clone, Default)]
pub struct Query {
    names: Vec<String>,
    root: Group,
    select: Option<Vec<Var>>,
    distinct: bool,
    order: Vec<SortKey>,
    limit: Option<usize>,
    as_of: Option<u64>,
    reduction_budget: Option<u64>,
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

    /// The query's top group.
    pub fn group(&mut self) -> &mut Group {
        &mut self.root
    }

    /// See [`Group::pattern`].
    pub fn pattern(
        &mut self,
        subject: impl Into<Term>,
        predicate: impl Into<Term>,
        object: impl Into<Term>,
    ) -> &mut Self {
        self.root.pattern(subject, predicate, object);
        self
    }

    /// See [`Group::link_pattern`].
    pub fn link_pattern(
        &mut self,
        link: Var,
        subject: impl Into<Term>,
        predicate: impl Into<Term>,
        object: impl Into<Term>,
    ) -> &mut Self {
        self.root.link_pattern(link, subject, predicate, object);
        self
    }

    /// See [`Group::at`].
    pub fn at(&mut self, id: u64) -> &mut Self {
        self.root.at(id);
        self
    }

    /// See [`Group::union`].
    pub fn union(&mut self, branches: impl IntoIterator<Item = Group>) -> &mut Self {
        self.root.union(branches);
        self
    }

    /// See [`Group::not`].
    pub fn not(&mut self, group: Group) -> &mut Self {
        self.root.not(group);
        self
    }

    /// See [`Group::filter`].
    pub fn filter(&mut self, filter: Filter) -> &mut Self {
        self.root.filter(filter);
        self
    }

    /// The variables each row returns, in order; by default, all those the
    /// query binds.
    pub fn select(&mut self, vars: impl IntoIterator<Item = Var>) -> &mut Self {
        self.select = Some(vars.into_iter().collect());
        self
    }

    /// Returns each selected row once.
    pub fn distinct(&mut self) -> &mut Self {
        self.distinct = true;
        self
    }

    /// Orders the rows by `key`, after any keys given before.
    pub fn order_by(&mut self, key: SortKey) -> &mut Self {
        self.order.push(key);
        self
    }

    /// Returns at most `n` rows.
    pub fn limit(&mut self, n: usize) -> &mut Self {
        self.limit = Some(n);
        self
    }

    /// Sees the graph as it was when `id` was appended, except in patterns
    /// given their own.
    pub fn as_of(&mut self, id: u64) -> &mut Self {
        self.as_of = Some(id);
        self
    }

    /// Limits the reduction of the top group to reading about `entries`
    /// index entries ([`DEFAULT_REDUCTION_BUDGET`] by default); 0 turns it
    /// off.
    pub fn reduction_budget(&mut self, entries: u64) -> &mut Self {
        self.reduction_budget = Some(entries);
        self
    }

    /// The patterns of the top group.
    pub fn patterns(&self) -> &[Pattern] {
        &self.root.patterns
    }

    /// Plans the query against `g`'s indexes and statistics.  Fails if a
    /// group has no pattern or union, the branches of a union bind different
    /// variables, or a variable used is not bound where it is used.
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
            .unwrap_or_else(|| self.root.bound_vars().into_iter().collect())
    }

    /// Checks the query's groups, and that the variables selected and
    /// ordered by are bound.
    fn check(&self) -> Result<()> {
        self.root.check()?;
        let bound = self.root.bound_vars();
        let used = self
            .selected()
            .into_iter()
            .chain(self.order.iter().map(|k| k.var));
        for v in used {
            if !bound.contains(&v) {
                return Err(Error::InvalidArgument(
                    "a variable selected or ordered by is not bound",
                ));
            }
        }
        Ok(())
    }

    fn var_name(&self, v: Var) -> String {
        format!("?{}", self.names[v.0])
    }

    fn term_name(&self, t: Term) -> String {
        match t {
            Term::Var(v) => self.var_name(v),
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
        let spo = match p.link {
            Some(l) => format!("{} = ({spo})", self.var_name(l)),
            None => format!("({spo})"),
        };
        match p.as_of {
            Some(id) => format!("{spo} as of {id}"),
            None => spo,
        }
    }

    fn filter_name(&self, f: &Filter) -> String {
        match f {
            Filter::Eq(a, b) => format!("{} = {}", self.term_name(*a), self.term_name(*b)),
            Filter::Ne(a, b) => format!("{} != {}", self.term_name(*a), self.term_name(*b)),
            Filter::Lt(a, b) => format!("{} < {}", self.term_name(*a), self.term_name(*b)),
            Filter::Compare(a, c, b) => format!(
                "data({}) {} data({})",
                self.term_name(*a),
                c.symbol(),
                self.term_name(*b)
            ),
            Filter::Test(v, _) => format!("test({})", self.var_name(*v)),
        }
    }

    fn sort_key_name(&self, k: &SortKey) -> String {
        let v = self.var_name(k.var);
        let key = if k.by_data { format!("data({v})") } else { v };
        if k.descending {
            format!("{key} desc")
        } else {
            key
        }
    }
}
