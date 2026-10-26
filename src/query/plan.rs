//! Cost-based planning of a [`Query`]; see the [module](super) docs.

use std::collections::HashMap;
use std::fmt::Write as _;

use super::{Filter, Group, Pattern, Query, Term, Var};
use crate::graph::{Error, Graph, Result};
use crate::record::Linkage;
use crate::tuple_index;

/// The most patterns and unions of a group planned exhaustively; more are
/// planned greedily.
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
/// The cost of starting to evaluate a group.
const GROUP_COST: f64 = 1.0;

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

/// A step of a group's plan, applied to the rows of the steps before it.
#[derive(Debug, Clone)]
pub(crate) enum Step {
    /// Joins a pattern: for a bind join, `access` is the lookup for each
    /// row; for a hash join, the scan, joined on `keys`.
    Pattern {
        pattern: Pattern,
        join: Join,
        access: Access,
        keys: Vec<Var>,
        rows: f64,
    },
    /// Joins a union, whose branches bind `vars`.  The branches' plans are
    /// for the rows so far, for a bind join, or alone, for a hash join on
    /// `keys`.
    Union {
        branches: Vec<GroupPlan>,
        vars: Vec<Var>,
        keys: Vec<Var>,
        join: Join,
        rows: f64,
    },
    Filter(Vec<Filter>),
    /// Removes the rows for which a group has a match, given their values of
    /// `keys`.  The group's plan is for the rows so far, for a bind join, or
    /// alone, for a hash join.
    Not {
        group: GroupPlan,
        keys: Vec<Var>,
        join: Join,
        rows: f64,
    },
}

/// The plan of a group.
#[derive(Debug, Clone)]
pub(crate) struct GroupPlan {
    pub steps: Vec<Step>,
    /// The estimated cost and rows for each row given.
    pub cost: f64,
    pub rows: f64,
}

/// A plan for running a [`Query`], made by [`Query::plan`].
#[derive(Debug, Clone)]
pub struct Plan {
    pub(crate) query: Query,
    pub(crate) root: GroupPlan,
}

impl Plan {
    /// The estimated cost of running the plan, in rough units of index
    /// entries read.
    pub fn estimated_cost(&self) -> f64 {
        self.root.cost
    }

    /// The estimated rows, before any limit or deduplication.
    pub fn estimated_rows(&self) -> f64 {
        self.root.rows
    }

    /// A description of the plan, a line per step.
    pub fn explain(&self) -> String {
        let q = &self.query;
        let mut out = String::new();
        explain_group(q, &self.root, 0, true, &mut out);
        if q.distinct {
            out.push_str("distinct\n");
        }
        if !q.order.is_empty() {
            let keys: Vec<String> = q.order.iter().map(|k| q.sort_key_name(k)).collect();
            let _ = writeln!(out, "order by {}", keys.join(", "));
        }
        if let Some(n) = q.limit {
            let _ = writeln!(out, "limit {n}");
        }
        out
    }
}

fn join_name(q: &Query, first: bool, join: Join, keys: &[Var]) -> String {
    match (first, join) {
        (true, _) => "scan".to_string(),
        (_, Join::Bind) => "bind join".to_string(),
        (_, Join::Hash) if keys.is_empty() => "hash join (cross product)".to_string(),
        (_, Join::Hash) => {
            let keys: Vec<String> = keys.iter().map(|&v| q.var_name(v)).collect();
            format!("hash join on {}", keys.join(", "))
        }
    }
}

/// Describes `plan`, whose rows start as a single row binding nothing if
/// `alone`.
fn explain_group(q: &Query, plan: &GroupPlan, depth: usize, alone: bool, out: &mut String) {
    let pad = "  ".repeat(depth);
    let mut first = alone;
    for step in &plan.steps {
        match step {
            Step::Pattern {
                pattern,
                join,
                access,
                keys,
                rows,
            } => {
                let _ = writeln!(
                    out,
                    "{pad}{} {} via {}, rows ~{rows:.0}",
                    join_name(q, first, *join, keys),
                    q.pattern_name(pattern),
                    access.name(),
                );
                first = false;
            }
            Step::Union {
                branches,
                keys,
                join,
                rows,
                ..
            } => {
                let _ = writeln!(
                    out,
                    "{pad}union, {}, rows ~{rows:.0}",
                    join_name(q, first, *join, keys)
                );
                for b in branches {
                    let _ = writeln!(out, "{pad}  branch:");
                    explain_group(q, b, depth + 2, *join == Join::Hash, out);
                }
                first = false;
            }
            Step::Filter(filters) => {
                for f in filters {
                    let _ = writeln!(out, "{pad}filter {}", q.filter_name(f));
                }
            }
            Step::Not {
                group,
                keys,
                join,
                rows,
            } => {
                let how = match join {
                    Join::Bind => "for the rows",
                    Join::Hash => "once",
                };
                let keys: Vec<String> = keys.iter().map(|&v| q.var_name(v)).collect();
                let _ = writeln!(
                    out,
                    "{pad}not ({how}, on {}), rows ~{rows:.0}:",
                    keys.join(", ")
                );
                explain_group(q, group, depth + 1, *join == Join::Hash, out);
            }
        }
    }
}

fn dedup(mut vars: Vec<Var>) -> Vec<Var> {
    vars.sort();
    vars.dedup();
    vars
}

/// The distinct variables of `pattern` that are `bound`.
pub(crate) fn shared_vars(pattern: &Pattern, bound: &[bool]) -> Vec<Var> {
    dedup(pattern.vars().filter(|v| bound[v.0]).collect())
}

/// Estimates of the links matching patterns.
struct Estimator<'g> {
    g: &'g Graph,
    /// The fraction of all links that are live.
    live_ratio: f64,
    /// Estimates of the distinct subjects and objects of all links.
    subjects: f64,
    objects: f64,
    /// The links matching the constants of a pattern at a subset of its
    /// positions (by bit), live and dead.
    counts: HashMap<(Pattern, u8), f64>,
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
    fn count(&mut self, pattern: &Pattern, mask: u8) -> f64 {
        if let Some(&c) = self.counts.get(&(*pattern, mask)) {
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
        self.counts.insert((*pattern, mask), c);
        c
    }

    /// Estimates a lookup of `pattern` with the variables `bound`.
    fn estimate(&mut self, pattern: &Pattern, bound: &[bool]) -> Estimate {
        if pattern.link.is_some_and(|v| bound[v.0]) {
            return Estimate {
                matches: 1.0,
                entries: 1.0,
            };
        }
        let terms = pattern.terms();
        let bits =
            |f: &dyn Fn(Term) -> bool| (0..3).fold(0u8, |m, k| m | ((f(terms[k]) as u8) << k));
        let consts = bits(&|t| matches!(t, Term::Id(_)));
        let vars = bits(&|t| t.var().is_some_and(|v| bound[v.0]));
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
            let mut n = self.count(pattern, mask & consts) * live_ratio;
            for (bit, distinct) in [(1, subjects), (2, predicates), (4, objects)] {
                if mask & vars & bit != 0 {
                    n /= distinct;
                }
            }
            n
        };
        let all = consts | vars;
        let access = Access::of(false, [0, 1, 2].map(|k| all & (1 << k) != 0));
        let key = (0..3).fold(0u8, |m, k| m | ((access.key()[k] as u8) << k));
        Estimate {
            matches: matching(all),
            entries: matching(key) / live_ratio.max(f64::MIN_POSITIVE),
        }
    }
}

fn ratio(a: u64, b: u64) -> f64 {
    if b == 0 { 1.0 } else { a as f64 / b as f64 }
}

/// A pattern or union of a group, to be joined.
#[derive(Clone, Copy)]
enum Item<'q> {
    Pattern(&'q Pattern),
    Union(&'q [Group]),
}

/// A partial plan of a group.
#[derive(Clone)]
struct Partial {
    cost: f64,
    rows: f64,
    steps: Vec<Step>,
    bound: Vec<bool>,
    /// Whether the rows are still the single row given to the group's plan
    /// with nothing bound, so that a hash join gains nothing.
    unit: bool,
    filters_done: Vec<bool>,
    nots_done: Vec<bool>,
}

struct Planner<'g> {
    est: Estimator<'g>,
    width: usize,
    /// For testing: the join to use wherever it can be.
    force: Option<Join>,
    /// The plans of groups (by address) for the variables bound.
    memo: HashMap<(usize, Vec<bool>), Option<GroupPlan>>,
}

impl Planner<'_> {
    fn unbound(&self) -> Vec<bool> {
        vec![false; self.width]
    }

    /// The plan of `group` for rows binding `bound`, or `None` if it has a
    /// filter or negation that can not be applied.
    fn plan_group(&mut self, group: &Group, bound: &[bool]) -> Option<GroupPlan> {
        let key = (group as *const Group as usize, bound.to_vec());
        if let Some(plan) = self.memo.get(&key) {
            return plan.clone();
        }
        let plan = self.plan_group_uncached(group, bound);
        self.memo.insert(key, plan.clone());
        plan
    }

    fn plan_group_uncached(&mut self, group: &Group, bound: &[bool]) -> Option<GroupPlan> {
        let items: Vec<Item> = group
            .patterns
            .iter()
            .map(Item::Pattern)
            .chain(group.unions.iter().map(|u| Item::Union(u)))
            .collect();
        // The variables bound once every item is joined.
        let mut bindable = bound.to_vec();
        group.bound_vars().iter().for_each(|v| bindable[v.0] = true);
        let start = Partial {
            cost: 0.0,
            rows: 1.0,
            steps: Vec::new(),
            bound: bound.to_vec(),
            unit: !bound.contains(&true),
            filters_done: vec![false; group.filters.len()],
            nots_done: vec![false; group.nots.len()],
        };
        let start = self.apply_ready(group, &bindable, start)?;
        let n = items.len();
        let best = if n <= EXHAUSTIVE_PATTERNS {
            let mut best: Vec<Option<Partial>> = vec![None; 1 << n];
            best[0] = Some(start);
            for mask in 0..(1usize << n) {
                let Some(partial) = best[mask].take() else {
                    continue;
                };
                for i in (0..n).filter(|&i| mask & (1 << i) == 0) {
                    for way in self.extend(group, &bindable, &partial, items[i]) {
                        let next = &mut best[mask | (1 << i)];
                        if next.as_ref().is_none_or(|b| way.cost < b.cost) {
                            *next = Some(way);
                        }
                    }
                }
                best[mask] = Some(partial);
            }
            best.pop().flatten()?
        } else {
            let mut partial = start;
            let mut left: Vec<Item> = items;
            while !left.is_empty() {
                let mut ways = Vec::new();
                for (i, &item) in left.iter().enumerate() {
                    for way in self.extend(group, &bindable, &partial, item) {
                        ways.push((i, way));
                    }
                }
                let (i, way) = ways
                    .into_iter()
                    .min_by(|a, b| a.1.cost.total_cmp(&b.1.cost))?;
                left.remove(i);
                partial = way;
            }
            partial
        };
        if best.filters_done.contains(&false) || best.nots_done.contains(&false) {
            return None;
        }
        Some(GroupPlan {
            steps: best.steps,
            cost: best.cost,
            rows: best.rows,
        })
    }

    /// Applies the filters and negations of `group` that are ready, or
    /// returns `None` if a negation ready can not be evaluated.
    fn apply_ready(&mut self, group: &Group, bindable: &[bool], mut p: Partial) -> Option<Partial> {
        let ready: Vec<usize> = (0..group.filters.len())
            .filter(|&f| !p.filters_done[f] && group.filters[f].vars().iter().all(|v| p.bound[v.0]))
            .collect();
        if !ready.is_empty() {
            p.cost += p.rows * FILTER_COST;
            for &f in &ready {
                p.filters_done[f] = true;
                p.rows *= group.filters[f].selectivity();
            }
            p.steps.push(Step::Filter(
                ready.iter().map(|&f| group.filters[f].clone()).collect(),
            ));
        }
        for (i, not) in group.nots.iter().enumerate() {
            // The negation applies once the variables it shares are bound.
            let keys: Vec<Var> = not
                .all_vars()
                .into_iter()
                .filter(|v| bindable[v.0])
                .collect();
            if p.nots_done[i] || !keys.iter().all(|v| p.bound[v.0]) {
                continue;
            }
            let bind = self.plan_group(not, &p.bound).map(|sub| {
                let cost = p.rows * sub.cost + GROUP_COST;
                (Join::Bind, sub, cost)
            });
            let unbound = self.unbound();
            let hash = self.plan_group(not, &unbound).map(|sub| {
                let cost = sub.cost + sub.rows * BUILD_COST + p.rows * PROBE_COST;
                (Join::Hash, sub, cost)
            });
            let matches = match (&bind, &hash) {
                (Some(b), _) => b.1.rows,
                (None, Some(h)) => h.1.rows,
                (None, None) => return None,
            };
            let ways: Vec<_> = [bind, hash].into_iter().flatten().collect();
            let forced = ways.iter().any(|w| Some(w.0) == self.force);
            let (join, sub, cost) = ways
                .into_iter()
                .filter(|w| !forced || Some(w.0) == self.force)
                .min_by(|a, b| a.2.total_cmp(&b.2))?;
            p.nots_done[i] = true;
            p.cost += cost;
            // The more matches for each row, the fewer rows are left.
            p.rows /= 1.0 + matches;
            p.steps.push(Step::Not {
                group: sub,
                keys,
                join,
                rows: p.rows,
            });
        }
        Some(p)
    }

    /// The ways of joining `item` to `partial`.
    fn extend(
        &mut self,
        group: &Group,
        bindable: &[bool],
        partial: &Partial,
        item: Item,
    ) -> Vec<Partial> {
        let mut after = partial.bound.clone();
        let unbound = self.unbound();
        // The ways: a step, its cost, and the rows after it.
        let mut ways: Vec<(Step, f64, f64)> = Vec::new();
        match item {
            Item::Pattern(pattern) => {
                pattern.vars().for_each(|v| after[v.0] = true);
                let bind = self.est.estimate(pattern, &partial.bound);
                let rows = partial.rows * bind.matches;
                let positions = |bound: &[bool]| {
                    pattern.terms().map(|t| match t {
                        Term::Id(_) => true,
                        Term::Var(v) => bound[v.0],
                    })
                };
                let link = pattern.link.is_some_and(|v| partial.bound[v.0]);
                let keys = shared_vars(pattern, &partial.bound);
                ways.push((
                    Step::Pattern {
                        pattern: *pattern,
                        join: Join::Bind,
                        access: Access::of(link, positions(&partial.bound)),
                        keys: keys.clone(),
                        rows,
                    },
                    partial.rows * (LOOKUP_COST + bind.entries * ENTRY_COST),
                    rows,
                ));
                if !partial.unit {
                    let scan = self.est.estimate(pattern, &unbound);
                    let cost = LOOKUP_COST
                        + scan.entries * ENTRY_COST
                        + scan.matches * BUILD_COST
                        + partial.rows * PROBE_COST;
                    ways.push((
                        Step::Pattern {
                            pattern: *pattern,
                            join: Join::Hash,
                            access: Access::of(false, positions(&unbound)),
                            keys,
                            rows,
                        },
                        cost,
                        rows,
                    ));
                }
            }
            Item::Union(branches) => {
                let vars: Vec<Var> = branches[0].bound_vars().into_iter().collect();
                vars.iter().for_each(|v| after[v.0] = true);
                let keys: Vec<Var> = vars
                    .iter()
                    .copied()
                    .filter(|v| partial.bound[v.0])
                    .collect();
                let bind: Option<Vec<GroupPlan>> = branches
                    .iter()
                    .map(|b| self.plan_group(b, &partial.bound))
                    .collect();
                let bind_rows = bind
                    .as_ref()
                    .map(|plans| partial.rows * plans.iter().map(|p| p.rows).sum::<f64>());
                if let (Some(plans), Some(rows)) = (bind, bind_rows) {
                    let cost = partial.rows * plans.iter().map(|p| p.cost).sum::<f64>()
                        + plans.len() as f64 * GROUP_COST;
                    ways.push((
                        Step::Union {
                            branches: plans,
                            vars: vars.clone(),
                            keys: keys.clone(),
                            join: Join::Bind,
                            rows,
                        },
                        cost,
                        rows,
                    ));
                }
                let alone: Option<Vec<GroupPlan>> = branches
                    .iter()
                    .map(|b| self.plan_group(b, &unbound))
                    .collect();
                if let Some(plans) = alone.filter(|_| !partial.unit) {
                    let built: f64 = plans.iter().map(|p| p.rows).sum();
                    let rows = bind_rows.unwrap_or(partial.rows * built);
                    let cost = plans.iter().map(|p| p.cost).sum::<f64>()
                        + built * BUILD_COST
                        + partial.rows * PROBE_COST;
                    ways.push((
                        Step::Union {
                            branches: plans,
                            vars,
                            keys,
                            join: Join::Hash,
                            rows,
                        },
                        cost,
                        rows,
                    ));
                }
            }
        }
        let join = |step: &Step| match step {
            Step::Pattern { join, .. } | Step::Union { join, .. } => Some(*join),
            _ => None,
        };
        let force = self.force;
        let forced = ways.iter().any(|w| join(&w.0) == force);
        ways.into_iter()
            .filter(|w| !forced || join(&w.0) == force)
            .filter_map(|(step, cost, rows)| {
                let mut p = partial.clone();
                p.steps.push(step);
                p.cost += cost;
                p.rows = rows;
                p.bound.clone_from(&after);
                p.unit = false;
                self.apply_ready(group, bindable, p)
            })
            .collect()
    }
}

pub(crate) fn plan(q: &Query, g: &Graph) -> Result<Plan> {
    plan_with(q, g, None)
}

/// Plans `q`, using the join `force` wherever it can be.
pub(crate) fn plan_with(q: &Query, g: &Graph, force: Option<Join>) -> Result<Plan> {
    q.check()?;
    let width = q.names.len();
    let mut planner = Planner {
        est: Estimator::new(g),
        width,
        force,
        memo: HashMap::new(),
    };
    let root = planner
        .plan_group(&q.root, &vec![false; width])
        .ok_or(Error::InvalidArgument(
            "a filter or negation uses a variable not bound where it is",
        ))?;
    Ok(Plan {
        query: q.clone(),
        root,
    })
}
