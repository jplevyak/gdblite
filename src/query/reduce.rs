//! Reduction of a group's patterns by constraint propagation, before they
//! are joined.
//!
//! Ported in spirit from the C++ GraQL evaluator (`EvalPlanner`): each
//! variable is a node constraint with a set of candidate ids (initially
//! anything), each pattern a link constraint with a set of candidate links.
//! A pattern with known candidates at one of its positions is evaluated
//! from them, by the cheapest of:
//!
//! - the subject+predicate or predicate+object index, for each pair of
//!   candidates, when the predicate and the other position are known;
//! - the index of one known position, for each of its candidates;
//!
//! keeping the live links that match every position.  Its links narrow the
//! candidates of its variables, and a narrowed variable narrows the links of
//! the patterns using it, and so on, until nothing changes (arc
//! consistency).  The candidates are then a superset of the values in any
//! row, and the links a superset of those any row uses, so the joins that
//! follow can use them in place of the indexes; for a group without cycles,
//! they are exactly what the rows use.
//!
//! Like the C++ size cutoff, a budget limits the index entries read: the
//! patterns with the fewest candidates to start from go first, and once the
//! budget is spent the reduction stops, leaving some patterns unevaluated.
//! What it found is still sound to use.

use std::collections::{BTreeSet, HashMap};
use std::sync::Arc;

use super::{Filter, Group, Pattern, Term, Var};
use crate::graph::Graph;
use crate::local_table::IdSet;
use crate::record::Linkage;
use crate::tuple_index::{self, Intersection};

/// A way of finding a pattern's candidate links.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Route {
    /// From the candidates of its link variable.
    Links,
    /// From the subject+predicate or predicate+object index, for each pair
    /// of candidates.
    Pair(Intersection),
    /// From the subject or object index, for each candidate.
    Single(Linkage),
    /// From the predicate index, for each candidate predicate.
    Predicates,
}

/// A live link: its id, subject, predicate and object.
pub(crate) type Link = (u64, [u64; 3]);

/// The budget of index entries read by a reduction, by default.
pub const DEFAULT_REDUCTION_BUDGET: u64 = 1 << 16;

/// The most pairs looked up in the subject+predicate or predicate+object
/// index for a pattern.
const MAX_PAIRS: u64 = 4096;

/// The latest link of each subject, predicate and object of `candidates`,
/// among those appended by `as_of`, if it is live.  `candidates` must hold
/// every link of each subject, predicate and object it holds; ids that are
/// not links are skipped.
pub(crate) fn live_links(g: &Graph, candidates: &IdSet, as_of: Option<u64>) -> Vec<Link> {
    let mut latest: HashMap<[u64; 3], (u64, bool)> = HashMap::new();
    for &id in candidates.range(..=as_of.unwrap_or(u64::MAX)) {
        let Ok(record) = g.get(id) else { continue };
        if let Some(link) = record.as_link() {
            let spo = [link.subject(), link.predicate(), link.object()];
            latest.insert(spo, (id, link.dead()));
        }
    }
    let mut live: Vec<Link> = latest
        .into_iter()
        .filter(|&(_, (_, dead))| !dead)
        .map(|(spo, (id, _))| (id, spo))
        .collect();
    live.sort_unstable();
    live
}

/// The value of a link at each field of a pattern: subject, predicate,
/// object, and the link's id.
fn fields(p: &Pattern, &(id, spo): &Link) -> [(Option<Term>, u64); 4] {
    [
        (Some(p.subject), spo[0]),
        (Some(p.predicate), spo[1]),
        (Some(p.object), spo[2]),
        (p.link.map(Term::Var), id),
    ]
}

/// The result of reducing a group.
#[derive(Debug, Clone)]
pub(crate) struct Reduction {
    /// The candidates of each variable, or `None` if unconstrained.
    pub vars: Vec<Option<IdSet>>,
    /// The candidate links of each pattern of the group, if it was
    /// evaluated, and the number of distinct values at each of its fields.
    pub links: Vec<Option<Reduced>>,
    /// Whether every pattern was evaluated within the budget.
    pub complete: bool,
    /// The index entries read.
    pub entries: u64,
}

/// The candidate links of a pattern.
#[derive(Debug, Clone)]
pub(crate) struct Reduced {
    pub links: Arc<Vec<Link>>,
    /// The distinct values of each field: subject, predicate, object, link.
    pub distinct: [usize; 4],
}

impl Reduced {
    pub fn new(p: &Pattern, links: Vec<Link>) -> Self {
        let mut distinct = [0; 4];
        for (k, d) in distinct.iter_mut().enumerate() {
            let values: BTreeSet<u64> = links.iter().map(|l| fields(p, l)[k].1).collect();
            *d = values.len();
        }
        Reduced {
            links: Arc::new(links),
            distinct,
        }
    }
}

struct Reducer<'a> {
    g: &'a Graph,
    group: &'a Group,
    as_of: Option<u64>,
    budget: u64,
    entries: u64,
    vars: Vec<Option<IdSet>>,
    links: Vec<Option<Vec<Link>>>,
    /// The filters of the group on a single variable, by variable.
    filters: HashMap<Var, Vec<&'a Filter>>,
    width: usize,
}

impl Reducer<'_> {
    /// The candidates at a term: a constant, a constrained variable, or
    /// `None` if unconstrained.
    fn known(&self, t: Term) -> Option<IdSet> {
        match t {
            Term::Id(id) => Some([id].into()),
            Term::Var(v) => self.vars[v.0].clone(),
        }
    }

    /// Reads the links with `id` at `linkage` (or with a pair of ids, by
    /// `intersection`) into `out`, or returns false if that would exceed the
    /// budget.
    fn read(&mut self, out: &mut IdSet, set: IdSet) -> bool {
        self.entries += set.len() as u64;
        out.extend(set);
        self.entries <= self.budget
    }

    /// Whether `link` matches pattern `i` given the candidates.
    fn matches(&self, p: &Pattern, link: &Link) -> bool {
        let mut set: Vec<(Var, u64)> = Vec::new();
        fields(p, link).iter().all(|&(term, value)| match term {
            None => true,
            Some(Term::Id(c)) => c == value,
            Some(Term::Var(v)) => {
                if let Some(&(_, prev)) = set.iter().find(|(w, _)| *w == v) {
                    return prev == value;
                }
                set.push((v, value));
                self.vars[v.0].as_ref().is_none_or(|s| s.contains(&value))
            }
        })
    }

    /// The ways of evaluating `p` now, with the index entries each is
    /// estimated to read; empty if none of its positions is known.
    fn routes(&self, p: &Pattern) -> Vec<(f64, Route)> {
        let stats = self.g.stats();
        if let Some(links) = p.link.and_then(|v| self.vars[v.0].as_ref()) {
            return vec![(links.len() as f64, Route::Links)];
        }
        let [s, pr, o] = p.terms().map(|t| self.known(t));
        let len = |x: &Option<IdSet>| x.as_ref().map(|x| x.len() as f64);
        // Per predicate: its links, and its links per subject and object.
        let per = |f: fn(&crate::stats::PredicateStats) -> f64| {
            pr.as_ref().map(|ps| {
                ps.iter()
                    .map(|&p| stats.predicate(p).map_or(0.0, f))
                    .sum::<f64>()
            })
        };
        let links = per(|s| s.links as f64);
        let per_subject = per(|s| s.links as f64 / s.subjects.len().max(1) as f64);
        let per_object = per(|s| s.links as f64 / s.objects.len().max(1) as f64);
        // Over all predicates, links per subject (object): most subjects
        // likely have the commonest predicate.
        let most = |f: fn(&crate::stats::PredicateStats) -> u64| {
            stats
                .predicates()
                .map(|(_, s)| f(s))
                .max()
                .unwrap_or(0)
                .max(1) as f64
        };
        let total = stats.links() as f64;
        let mut routes = Vec::new();
        let pairs = |x: &Option<IdSet>| pr.as_ref().zip(x.as_ref()).map(|(p, x)| p.len() * x.len());
        if let (Some(n), Some(per)) = (pairs(&s), per_subject)
            && n as u64 <= MAX_PAIRS
        {
            let x = len(&s).unwrap();
            routes.push((
                x * per + n as f64,
                Route::Pair(Intersection::SubjectPredicate),
            ));
        }
        if let (Some(n), Some(per)) = (pairs(&o), per_object)
            && n as u64 <= MAX_PAIRS
        {
            let x = len(&o).unwrap();
            routes.push((
                x * per + n as f64,
                Route::Pair(Intersection::PredicateObject),
            ));
        }
        // The subject (object) index has every predicate's links, so at
        // least the pattern's predicate's.
        if let Some(x) = len(&s) {
            let per = (total / most(|s| s.subjects.len())).max(per_subject.unwrap_or(0.0));
            routes.push((x * (1.0 + per), Route::Single(Linkage::Subject)));
        }
        if let Some(x) = len(&o) {
            let per = (total / most(|s| s.objects.len())).max(per_object.unwrap_or(0.0));
            routes.push((x * (1.0 + per), Route::Single(Linkage::Object)));
        }
        if let Some(links) = links {
            routes.push((links, Route::Predicates));
        }
        routes
    }

    /// The cheapest way of evaluating `p` now, if any.
    fn route(&self, p: &Pattern) -> Option<(f64, Route)> {
        self.routes(p)
            .into_iter()
            .min_by(|a, b| a.0.total_cmp(&b.0))
    }

    /// The candidate links of pattern `p`, or `None` if the budget would be
    /// exceeded.
    fn evaluate(&mut self, p: &Pattern) -> Option<Vec<Link>> {
        let g = self.g;
        let local = g.local();
        let mut ids = IdSet::new();
        let [s, pr, o] = p.terms().map(|t| self.known(t));
        match self.route(p).map(|(_, r)| r) {
            Some(Route::Links) => {
                let links = self.vars[p.link.unwrap().0].clone().unwrap();
                for id in links {
                    let fields = g.get(id).ok().and_then(|r| r.as_link().map(|l| l.fields()));
                    if let Some(f) = fields {
                        let set =
                            tuple_index::find_links_by_spo(local, f.subject, f.predicate, f.object);
                        if !self.read(&mut ids, set) {
                            return None;
                        }
                    }
                }
            }
            Some(Route::Pair(intersection)) => {
                let x = match intersection {
                    Intersection::SubjectPredicate => s.unwrap(),
                    Intersection::PredicateObject => o.unwrap(),
                };
                for &p in pr.as_ref().unwrap() {
                    for &x in &x {
                        let (a, b) = match intersection {
                            Intersection::SubjectPredicate => (x, p),
                            Intersection::PredicateObject => (p, x),
                        };
                        let set =
                            tuple_index::find_links_by_intersection(local, intersection, a, b);
                        if !self.read(&mut ids, set) {
                            return None;
                        }
                    }
                }
            }
            Some(Route::Single(linkage)) => {
                let x = if linkage == Linkage::Subject { s } else { o };
                for id in x.unwrap() {
                    if !self.read(&mut ids, tuple_index::find_links(local, linkage, id)) {
                        return None;
                    }
                }
            }
            route @ (Some(Route::Predicates) | None) => {
                let predicates: Vec<u64> = match (route, &pr) {
                    (Some(_), Some(ps)) => ps.iter().copied().collect(),
                    _ => g.stats().predicates().map(|(p, _)| p).collect(),
                };
                for p in predicates {
                    // Check the size first: a predicate may have many links.
                    let n = g.stats().predicate(p).map_or(0, |s| s.links);
                    if self.entries + n > self.budget {
                        self.entries += n;
                        return None;
                    }
                    let set = tuple_index::find_links(local, Linkage::Predicate, p);
                    if !self.read(&mut ids, set) {
                        return None;
                    }
                }
            }
        }
        let as_of = p.as_of.or(self.as_of);
        let mut links = live_links(g, &ids, as_of);
        links.retain(|l| self.matches(p, l));
        Some(links)
    }

    /// Narrows the candidates of `v` to `values` and the filters on it,
    /// returning whether they changed.
    fn narrow(&mut self, v: Var, values: IdSet) -> bool {
        let mut values = match &self.vars[v.0] {
            Some(old) => old.intersection(&values).copied().collect(),
            None => values,
        };
        if let Some(filters) = self.filters.get(&v) {
            let mut row = vec![0; self.width];
            values.retain(|&id| {
                row[v.0] = id;
                filters.iter().all(|f| f.eval(self.g, &row))
            });
        }
        let changed = self.vars[v.0]
            .as_ref()
            .is_none_or(|old| old.len() != values.len());
        self.vars[v.0] = Some(values);
        changed
    }

    /// Narrows the variables of pattern `i` to the values of its links,
    /// returning those that changed.
    fn project(&mut self, i: usize) -> Vec<Var> {
        let p = self.group.patterns[i];
        let mut values: HashMap<Var, IdSet> = HashMap::new();
        for link in self.links[i].as_ref().unwrap() {
            for (term, value) in fields(&p, link) {
                if let Some(Term::Var(v)) = term {
                    values.entry(v).or_default().insert(value);
                }
            }
        }
        for v in p.vars() {
            values.entry(v).or_default();
        }
        let mut changed = Vec::new();
        for (v, set) in values {
            if self.narrow(v, set) {
                changed.push(v);
            }
        }
        changed
    }

    fn run(&mut self) -> bool {
        let n = self.group.patterns.len();
        let mut queue: Vec<Var> = Vec::new();
        loop {
            // Propagate narrowed variables to the patterns evaluated.
            while let Some(v) = queue.pop() {
                for i in 0..n {
                    let p = self.group.patterns[i];
                    let Some(links) = &self.links[i] else {
                        continue;
                    };
                    if !p.vars().any(|w| w == v) {
                        continue;
                    }
                    let before = links.len();
                    let kept: Vec<Link> = links
                        .iter()
                        .copied()
                        .filter(|l| self.matches(&p, l))
                        .collect();
                    if kept.len() != before {
                        self.links[i] = Some(kept);
                        queue.extend(self.project(i));
                    }
                }
            }
            if self.vars.iter().flatten().any(BTreeSet::is_empty) {
                return true;
            }
            // Evaluate the cheapest pattern left.
            let next = (0..n)
                .filter(|&i| self.links[i].is_none())
                .filter_map(|i| Some((self.route(&self.group.patterns[i])?.0, i)))
                .min_by(|a, b| a.0.total_cmp(&b.0))
                .map(|(_, i)| i)
                .or_else(|| (0..n).find(|&i| self.links[i].is_none()));
            let Some(i) = next else {
                return true;
            };
            let p = self.group.patterns[i];
            let Some(links) = self.evaluate(&p) else {
                return false;
            };
            self.links[i] = Some(links);
            queue.extend(self.project(i));
        }
    }
}

/// Reduces the patterns of `group`, whose variables are numbered below
/// `width`, reading at most about `budget` index entries.
/// `preset` gives the links of some patterns, by index, in place of
/// evaluating them.
pub(crate) fn reduce(
    g: &Graph,
    group: &Group,
    width: usize,
    as_of: Option<u64>,
    budget: u64,
    preset: &HashMap<usize, Vec<Link>>,
) -> Reduction {
    let mut filters: HashMap<Var, Vec<&Filter>> = HashMap::new();
    for f in &group.filters {
        let vars: BTreeSet<Var> = f.vars().into_iter().collect();
        if let [v] = vars.into_iter().collect::<Vec<_>>()[..] {
            filters.entry(v).or_default().push(f);
        }
    }
    let mut r = Reducer {
        g,
        group,
        as_of,
        budget,
        entries: 0,
        vars: vec![None; width],
        links: vec![None; group.patterns.len()],
        filters,
        width: width.max(1),
    };
    for (&i, links) in preset {
        r.links[i] = Some(links.clone());
        r.project(i);
    }
    let complete = r.run();
    let links = r
        .links
        .into_iter()
        .zip(&group.patterns)
        .map(|(links, p)| links.map(|links| Reduced::new(p, links)))
        .collect();
    Reduction {
        vars: r.vars,
        links,
        complete,
        entries: r.entries,
    }
}
