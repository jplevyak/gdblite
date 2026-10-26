//! Cost-based planning of a [`Query`]; see the [module](super) docs.

use std::collections::HashMap;
use std::fmt::Write as _;

use super::{Filter, Pattern, Query, Term, Var};
use crate::graph::{Graph, Result};
use crate::record::Linkage;
use crate::tuple_index;

/// The most patterns planned exhaustively; more are planned greedily.
pub const EXHAUSTIVE_PATTERNS: usize = 12;

/// The cost of an index lookup, apart from the entries it reads.
const LOOKUP_COST: f64 = 4.0;
/// The cost of reading an index entry and the link it names.
const ENTRY_COST: f64 = 1.0;
/// The cost of adding a row to a hash table.
const BUILD_COST: f64 = 2.0;
/// The cost of probing a hash table with a row.
const PROBE_COST: f64 = 1.0;
/// The cost of testing a row with a filter.
const FILTER_COST: f64 = 0.2;

/// The index a pattern is looked up in, given what is bound.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Access {
    /// The link is bound: read it.
    Link,
    Spo,
    SubjectPredicate,
    PredicateObject,
    Subject,
    Predicate,
    Object,
    /// Nothing is bound: every predicate's links.
    All,
}

impl Access {
    /// The access for a pattern with these positions bound.
    pub(crate) fn of(link: bool, [s, p, o]: [bool; 3]) -> Access {
        if link {
            return Access::Link;
        }
        match (s, p, o) {
            (true, true, true) => Access::Spo,
            (true, true, false) => Access::SubjectPredicate,
            (false, true, true) => Access::PredicateObject,
            (true, false, _) => Access::Subject,
            (false, true, false) => Access::Predicate,
            (false, false, true) => Access::Object,
            (false, false, false) => Access::All,
        }
    }

    /// The positions of the index's key.
    fn key(self) -> [bool; 3] {
        match self {
            Access::Link => [false; 3],
            Access::Spo => [true; 3],
            Access::SubjectPredicate => [true, true, false],
            Access::PredicateObject => [false, true, true],
            Access::Subject => [true, false, false],
            Access::Predicate => [false, true, false],
            Access::Object => [false, false, true],
            Access::All => [false; 3],
        }
    }

    fn name(self) -> &'static str {
        match self {
            Access::Link => "link",
            Access::Spo => "spo index",
            Access::SubjectPredicate => "subject+predicate index",
            Access::PredicateObject => "predicate+object index",
            Access::Subject => "subject index",
            Access::Predicate => "predicate index",
            Access::Object => "object index",
            Access::All => "all links",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Join {
    Bind,
    Hash,
}

/// One pattern of a plan, joined to the rows of the steps before it.
#[derive(Debug, Clone)]
pub(crate) struct Step {
    pub pattern: usize,
    pub join: Join,
    /// For a bind join, the lookup for each row; for a hash join, the scan.
    pub access: Access,
    /// The filters applied after this step.
    pub filters: Vec<usize>,
    /// The estimated rows after this step.
    pub rows: f64,
}

/// A plan for running a [`Query`], made by [`Query::plan`].
#[derive(Debug, Clone)]
pub struct Plan {
    pub(crate) query: Query,
    pub(crate) steps: Vec<Step>,
    /// The filters with no variables, applied first.
    pub(crate) initial_filters: Vec<usize>,
    cost: f64,
}

impl Plan {
    /// The estimated cost of running the plan, in rough units of index
    /// entries read.
    pub fn estimated_cost(&self) -> f64 {
        self.cost
    }

    /// The estimated rows, before any limit.
    pub fn estimated_rows(&self) -> f64 {
        self.steps.last().map_or(0.0, |s| s.rows)
    }

    /// A description of the plan, a line per step.
    pub fn explain(&self) -> String {
        let q = &self.query;
        let mut out = String::new();
        for &f in &self.initial_filters {
            let _ = writeln!(out, "filter {}", q.filter_name(&q.filters[f]));
        }
        let mut bound = vec![false; q.names.len()];
        for (i, step) in self.steps.iter().enumerate() {
            let pattern = &q.patterns[step.pattern];
            let join = match (i, step.join) {
                (0, _) => "scan".to_string(),
                (_, Join::Bind) => "bind join".to_string(),
                (_, Join::Hash) => {
                    let keys: Vec<String> = shared_vars(pattern, &bound)
                        .into_iter()
                        .map(|v| format!("?{}", q.names[v.0]))
                        .collect();
                    format!("hash join on {}", keys.join(", "))
                }
            };
            let _ = writeln!(
                out,
                "{join} {} via {}, rows ~{:.0}",
                q.pattern_name(pattern),
                step.access.name(),
                step.rows
            );
            for &f in &step.filters {
                let _ = writeln!(out, "  filter {}", q.filter_name(&q.filters[f]));
            }
            pattern.vars().for_each(|v| bound[v.0] = true);
        }
        if let Some(n) = q.limit {
            let _ = writeln!(out, "limit {n}");
        }
        out
    }
}

/// The distinct variables of `pattern` that are `bound`.
pub(crate) fn shared_vars(pattern: &Pattern, bound: &[bool]) -> Vec<Var> {
    let mut vars: Vec<Var> = pattern.vars().filter(|v| bound[v.0]).collect();
    vars.sort();
    vars.dedup();
    vars
}

/// Estimates of the links matching patterns.
struct Estimator<'g> {
    g: &'g Graph,
    /// The fraction of all links that are live.
    live_ratio: f64,
    /// Estimates of the distinct subjects and objects of all links.
    subjects: f64,
    objects: f64,
    /// For each pattern, the links matching its constants by each subset of
    /// its positions (by bit), live and dead.
    counts: HashMap<(usize, u8), f64>,
}

/// An estimate of a lookup: the live links matching, and the index entries
/// read.
#[derive(Debug, Clone, Copy)]
struct Estimate {
    matches: f64,
    entries: f64,
}

impl<'g> Estimator<'g> {
    fn new(g: &'g Graph) -> Self {
        let stats = g.stats();
        let max = |f: fn(&crate::stats::PredicateStats) -> u64| {
            stats.predicates().map(|(_, s)| f(s)).max().unwrap_or(0) as f64
        };
        Estimator {
            g,
            live_ratio: ratio(stats.live(), stats.links()),
            // Most subjects (objects) likely have the commonest predicate.
            subjects: max(|s| s.subjects.len()).max(1.0),
            objects: max(|s| s.objects.len()).max(1.0),
            counts: HashMap::new(),
        }
    }

    /// The links, live and dead, matching the constants of `pattern` at the
    /// positions in `mask` (bit 0 subject, 1 predicate, 2 object).
    fn count(&mut self, i: usize, pattern: &Pattern, mask: u8) -> f64 {
        if let Some(&c) = self.counts.get(&(i, mask)) {
            return c;
        }
        let id = |t: Term, bit: u8| match t {
            Term::Id(id) if mask & bit != 0 => Some(id),
            _ => None,
        };
        let (s, p, o) = (
            id(pattern.subject, 1),
            id(pattern.predicate, 2),
            id(pattern.object, 4),
        );
        let g = self.g;
        let local = g.local();
        let stats = g.stats();
        let c = match (s, p, o) {
            (Some(s), Some(p), Some(o)) => {
                tuple_index::find_links_by_spo(local, s, p, o).len() as u64
            }
            (Some(s), Some(p), None) => g.property_count(s, Linkage::Subject, p).unwrap_or(0),
            (None, Some(p), Some(o)) => g.property_count(o, Linkage::Object, p).unwrap_or(0),
            (Some(s), None, Some(o)) => g
                .link_count(Linkage::Subject, s)
                .min(g.link_count(Linkage::Object, o)),
            (Some(s), None, None) => g.link_count(Linkage::Subject, s),
            (None, None, Some(o)) => g.link_count(Linkage::Object, o),
            (None, Some(p), None) => stats.predicate(p).map_or(0, |s| s.links),
            (None, None, None) => stats.links(),
        } as f64;
        self.counts.insert((i, mask), c);
        c
    }

    /// Estimates a lookup of `pattern` with the positions `bound` (by bit,
    /// as for `count`) bound to variables and the link bound if `link`.
    fn estimate(&mut self, i: usize, pattern: &Pattern, link: bool, bound: u8) -> Estimate {
        if link {
            return Estimate {
                matches: 1.0,
                entries: 1.0,
            };
        }
        let terms = pattern.terms();
        let consts = (0..3).fold(0u8, |m, k| {
            m | if matches!(terms[k], Term::Id(_)) {
                1 << k
            } else {
                0
            }
        });
        let pstats = match pattern.predicate {
            Term::Id(p) => Some(self.g.stats().predicate(p).cloned().unwrap_or_default()),
            Term::Var(_) => None,
        };
        let (live_ratio, subjects, objects) = match &pstats {
            Some(s) => (
                ratio(s.live(), s.links),
                s.subjects.len().max(1) as f64,
                s.objects.len().max(1) as f64,
            ),
            None => (self.live_ratio, self.subjects, self.objects),
        };
        let predicates = self.g.stats().predicate_count().max(1) as f64;
        // The live links matching the positions in `mask`.
        let mut matching = |mask: u8| {
            let mut n = self.count(i, pattern, mask & consts) * live_ratio;
            let vars = mask & bound;
            for (bit, distinct) in [(1, subjects), (2, predicates), (4, objects)] {
                if vars & bit != 0 {
                    n /= distinct;
                }
            }
            n
        };
        let positions = |b: [bool; 3]| (0..3).fold(0u8, |m, k| m | ((b[k] as u8) << k));
        let all = consts | bound;
        let access = Access::of(false, [all & 1 != 0, all & 2 != 0, all & 4 != 0]);
        let key = positions(access.key());
        Estimate {
            matches: matching(all),
            entries: matching(key) / live_ratio.max(f64::MIN_POSITIVE),
        }
    }
}

fn ratio(a: u64, b: u64) -> f64 {
    if b == 0 { 1.0 } else { a as f64 / b as f64 }
}

/// A partial plan: the steps joining a subset of the patterns.
#[derive(Clone)]
struct Partial {
    cost: f64,
    rows: f64,
    steps: Vec<Step>,
}

struct Planner<'q, 'g> {
    q: &'q Query,
    est: Estimator<'g>,
}

impl Planner<'_, '_> {
    /// Whether each variable is bound by the patterns in `mask`.
    fn bound(&self, mask: u64) -> Vec<bool> {
        let mut bound = vec![false; self.q.names.len()];
        for (i, p) in self.q.patterns.iter().enumerate() {
            if mask & (1 << i) != 0 {
                p.vars().for_each(|v| bound[v.0] = true);
            }
        }
        bound
    }

    /// The filters that apply once `bound` is, but not before `before`.
    fn new_filters(&self, before: &[bool], bound: &[bool]) -> Vec<usize> {
        let applies = |b: &[bool], f: &Filter| {
            let vars = f.vars();
            !vars.is_empty() && vars.iter().all(|v| b[v.0])
        };
        (0..self.q.filters.len())
            .filter(|&f| {
                let f = &self.q.filters[f];
                applies(bound, f) && !applies(before, f)
            })
            .collect()
    }

    /// The ways of joining pattern `i` to `partial`, over the patterns in
    /// `mask`.
    fn extend(&mut self, partial: &Partial, mask: u64, i: usize) -> Vec<Partial> {
        let pattern = self.q.patterns[i];
        let before = self.bound(mask);
        let after = self.bound(mask | (1 << i));
        let is_bound = |t: Term| t.var().is_some_and(|v| before[v.0]);
        let bits = pattern
            .terms()
            .iter()
            .enumerate()
            .fold(0u8, |m, (k, &t)| m | ((is_bound(t) as u8) << k));
        let link = pattern.link.is_some_and(|v| before[v.0]);
        let bind = self.est.estimate(i, &pattern, link, bits);
        let filters = self.new_filters(&before, &after);
        let selectivity: f64 = filters
            .iter()
            .map(|&f| self.q.filters[f].selectivity())
            .product();
        let rows = partial.rows * bind.matches;
        let out_rows = rows * selectivity;
        let filter_cost = if filters.is_empty() {
            0.0
        } else {
            rows * FILTER_COST
        };
        let terms = pattern.terms();
        let bound_positions =
            |extra: u8| [0, 1, 2].map(|k| matches!(terms[k], Term::Id(_)) || (extra >> k) & 1 != 0);
        let step = |join, access, cost: f64| {
            let mut steps = partial.steps.clone();
            steps.push(Step {
                pattern: i,
                join,
                access,
                filters: filters.clone(),
                rows: out_rows,
            });
            Partial {
                cost: partial.cost + cost + filter_cost,
                rows: out_rows,
                steps,
            }
        };
        let mut ways = vec![step(
            Join::Bind,
            Access::of(link, bound_positions(bits)),
            partial.rows * (LOOKUP_COST + bind.entries * ENTRY_COST),
        )];
        if !partial.steps.is_empty() && !shared_vars(&pattern, &before).is_empty() {
            let scan = self.est.estimate(i, &pattern, false, 0);
            let cost = LOOKUP_COST
                + scan.entries * ENTRY_COST
                + scan.matches * BUILD_COST
                + partial.rows * PROBE_COST;
            ways.push(step(
                Join::Hash,
                Access::of(false, bound_positions(0)),
                cost,
            ));
        }
        ways
    }

    fn cheapest(ways: impl IntoIterator<Item = Partial>) -> Option<Partial> {
        ways.into_iter().min_by(|a, b| a.cost.total_cmp(&b.cost))
    }

    /// The cheapest left-deep plan, over every subset of the patterns.
    fn exhaustive(&mut self) -> Partial {
        let n = self.q.patterns.len();
        let mut best: Vec<Option<Partial>> = vec![None; 1 << n];
        best[0] = Some(Partial {
            cost: 0.0,
            rows: 1.0,
            steps: Vec::new(),
        });
        for mask in 0..(1u64 << n) {
            let Some(partial) = best[mask as usize].take() else {
                continue;
            };
            for i in (0..n).filter(|&i| mask & (1 << i) == 0) {
                let next = (mask | (1 << i)) as usize;
                for way in self.extend(&partial, mask, i) {
                    if best[next].as_ref().is_none_or(|b| way.cost < b.cost) {
                        best[next] = Some(way);
                    }
                }
            }
            best[mask as usize] = Some(partial);
        }
        best.pop().flatten().expect("a plan")
    }

    /// A plan adding the cheapest next pattern at each step.
    fn greedy(&mut self) -> Partial {
        let n = self.q.patterns.len();
        let mut partial = Partial {
            cost: 0.0,
            rows: 1.0,
            steps: Vec::new(),
        };
        let mut mask = 0u64;
        for _ in 0..n {
            let ways: Vec<Partial> = (0..n)
                .filter(|&i| mask & (1 << i) == 0)
                .flat_map(|i| self.extend(&partial, mask, i))
                .collect();
            partial = Self::cheapest(ways).expect("a pattern left");
            mask |= 1 << partial.steps.last().unwrap().pattern;
        }
        partial
    }
}

pub(crate) fn plan(q: &Query, g: &Graph) -> Result<Plan> {
    q.check()?;
    let initial_filters = (0..q.filters.len())
        .filter(|&f| q.filters[f].vars().is_empty())
        .collect();
    let mut planner = Planner {
        q,
        est: Estimator::new(g),
    };
    let best = if q.patterns.len() <= EXHAUSTIVE_PATTERNS {
        planner.exhaustive()
    } else {
        planner.greedy()
    };
    Ok(Plan {
        query: q.clone(),
        steps: best.steps,
        initial_filters,
        cost: best.cost,
    })
}
