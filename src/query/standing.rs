//! Standing queries, whose rows are kept up to date as the graph changes.
//!
//! A [`StandingQuery`] evaluates a [`Query`] once, then on each
//! [`update`](StandingQuery::update) works out the changes to its rows from
//! the links committed since, without evaluating it again.
//!
//! # How
//!
//! Ids increase as entries are appended, so the entries committed since the
//! last update are those from its id on, and each pattern can see the graph
//! as of any id.  The links that changed are the latest links of the
//! subjects, predicates and objects of those entries, as of then and now:
//! each is *added*, if live now and not then, or *removed*, if live then and
//! not now.
//!
//! A query's rows (as a bag) are a sum of products of its patterns' links,
//! so for patterns P1 ... Pn the change in its rows is the sum over i of the
//! query with P1 ... Pi-1 as of now, Pi replaced by its added links (counting
//! +1) or its removed links (counting -1), and Pi+1 ... Pn as of then, and
//! each union that Pi is in reduced to the branch it is in (the other
//! branches add rows that do not change with it).  Each term is an ordinary
//! query, planned and run as usual, and the reduction propagates the few
//! changed links to the rest of it.
//!
//! A negated group N is one more term: its rows are those of the query
//! passing a test, whether N has no match, that changes only for the rows
//! whose values for N's variables are among N's own changes.  Those rows
//! are found from the changes to N alone (without its conditions on
//! variables it does not bind, so as to find all of them), and each is
//! tested against N as of then and now.
//!
//! Patterns given their own `as_of` do not change.  Only committed entries
//! are seen, so that a rollback can not leave rows behind.

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::sync::Arc;

use super::plan::{self, NotChange, Overrides};
use super::reduce::{Link, live_links};
use super::{Group, Pattern, Query, Term, Var};
use crate::graph::{Error, Graph, Result};
use crate::tuple_index;

/// Changes to the rows of a [`StandingQuery`]: each row added or removed,
/// with how many times.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Changes {
    pub added: Vec<(Vec<u64>, u64)>,
    pub removed: Vec<(Vec<u64>, u64)>,
}

impl Changes {
    pub fn is_empty(&self) -> bool {
        self.added.is_empty() && self.removed.is_empty()
    }
}

/// A query whose rows are kept up to date as entries are committed.
///
/// [`update`](Self::update) works out the changes from the links committed
/// since the last update, rather than evaluating the query again: one term
/// per pattern, joining that pattern's changed links to the other patterns
/// as of the last update or now, and one per negated group, testing the rows
/// its own changes could affect.  Only committed entries are seen, and
/// patterns given their own `as_of` are taken not to change.
#[derive(Debug, Clone)]
pub struct StandingQuery {
    /// The query, selecting its variables explicitly and not distinct.
    query: Query,
    distinct: bool,
    /// The committed id the rows are as of the entries before.
    at: u64,
    /// Each row, and how many times it occurs.
    counts: BTreeMap<Vec<u64>, u64>,
}

/// The id following the last committed entry.
fn committed(g: &Graph) -> u64 {
    g.local().log().metadata().next_id
}

impl StandingQuery {
    /// Evaluates `query` on the committed entries of `g`.  Fails if it has a
    /// limit, an order, an `as_of`, paths or calls, which a standing query
    /// can not keep.
    pub fn new(query: &Query, g: &Graph) -> Result<Self> {
        if query.limit.is_some() || !query.order.is_empty() || query.as_of.is_some() {
            return Err(Error::InvalidArgument(
                "a standing query can not have a limit, order or as_of",
            ));
        }
        if query.root.has_paths_or_calls() {
            return Err(Error::InvalidArgument(
                "a standing query can not have paths or calls",
            ));
        }
        let mut q = query.clone();
        q.select = Some(query.selected());
        q.distinct = false;
        let at = committed(g);
        let mut first = q.clone();
        first.as_of = Some(at - 1);
        let mut counts = BTreeMap::new();
        for row in first.run(g)? {
            *counts.entry(row).or_default() += 1;
        }
        Ok(StandingQuery {
            query: q,
            distinct: query.distinct,
            at,
            counts,
        })
    }

    /// The id the rows reflect the committed entries before.
    pub fn at(&self) -> u64 {
        self.at
    }

    /// The query's rows, in order.
    pub fn rows(&self) -> Vec<Vec<u64>> {
        let mut rows = Vec::new();
        for (row, &n) in &self.counts {
            let n = if self.distinct { 1 } else { n };
            rows.extend(std::iter::repeat_n(row.clone(), n as usize));
        }
        rows
    }

    /// Brings the rows up to date with the entries committed to `g`,
    /// returning the changes.
    pub fn update(&mut self, g: &Graph) -> Result<Changes> {
        let now = committed(g);
        if now == self.at {
            return Ok(Changes::default());
        }
        let delta = Delta::new(g, self.at, now)?;
        let changes = if delta.added.is_empty() && delta.removed.is_empty() {
            HashMap::new()
        } else {
            row_changes(&self.query, g, &delta)?
        };
        let mut out = Changes::default();
        for (row, change) in changes {
            if change == 0 {
                continue;
            }
            let count = self.counts.entry(row.clone()).or_default();
            let before = *count;
            *count = before
                .checked_add_signed(change)
                .ok_or(Error::InvalidArgument(
                    "a standing query's rows fell below zero",
                ))?;
            let after = *count;
            if after == 0 {
                self.counts.remove(&row);
            }
            if self.distinct {
                match (before, after) {
                    (0, a) if a > 0 => out.added.push((row, 1)),
                    (b, 0) if b > 0 => out.removed.push((row, 1)),
                    _ => {}
                }
            } else if change > 0 {
                out.added.push((row, change as u64));
            } else {
                out.removed.push((row, change.unsigned_abs()));
            }
        }
        out.added.sort();
        out.removed.sort();
        self.at = now;
        Ok(out)
    }
}

/// The links that changed between two times.
struct Delta {
    /// The `as_of` ids of then and now.
    old: u64,
    new: u64,
    added: Vec<Link>,
    removed: Vec<Link>,
}

impl Delta {
    /// The changes made by the entries from `from` to before `to`.
    fn new(g: &Graph, from: u64, to: u64) -> Result<Self> {
        let (old, new) = (from - 1, to - 1);
        let mut spos: BTreeSet<[u64; 3]> = BTreeSet::new();
        for record in g.records(from) {
            let record = record?;
            if record.id() >= to {
                break;
            }
            if let Some(l) = record.as_link() {
                spos.insert([l.subject(), l.predicate(), l.object()]);
            }
        }
        let (mut added, mut removed) = (Vec::new(), Vec::new());
        for [s, p, o] in spos {
            let links = tuple_index::find_links_by_spo(g.local(), s, p, o);
            let live = |as_of| {
                live_links(g, &links, Some(as_of))
                    .into_iter()
                    .find(|l| l.1 == [s, p, o])
            };
            let (then, now) = (live(old), live(new));
            if then != now {
                added.extend(now);
                removed.extend(then);
            }
        }
        Ok(Delta {
            old,
            new,
            added,
            removed,
        })
    }
}

/// A place in a query's groups: the union and branch taken at each level.
type Path = Vec<(usize, usize)>;

fn group_at<'a>(group: &'a Group, path: &[(usize, usize)]) -> &'a Group {
    path.iter().fold(group, |g, &(u, b)| &g.unions[u][b])
}

fn group_at_mut<'a>(group: &'a mut Group, path: &[(usize, usize)]) -> &'a mut Group {
    path.iter().fold(group, |g, &(u, b)| &mut g.unions[u][b])
}

/// The parts of a query whose changes change its rows.
#[derive(Debug, Clone)]
enum Arg {
    /// A pattern that does not have its own `as_of`.
    Pattern(Path, usize),
    /// A negated group.
    Not(Path, usize),
}

fn args(group: &Group, path: &mut Path, out: &mut Vec<Arg>) {
    for (i, p) in group.patterns.iter().enumerate() {
        if p.as_of.is_none() {
            out.push(Arg::Pattern(path.clone(), i));
        }
    }
    for (u, branches) in group.unions.iter().enumerate() {
        for (b, branch) in branches.iter().enumerate() {
            path.push((u, b));
            args(branch, path, out);
            path.pop();
        }
    }
    for k in 0..group.nots.len() {
        out.push(Arg::Not(path.clone(), k));
    }
}

/// Keeps, of each union on `path` from `group`, only the branch on it,
/// returning the path to the same group then.
fn isolate(group: &mut Group, path: &[(usize, usize)]) -> Path {
    let mut g = group;
    let mut isolated = Vec::new();
    for &(u, b) in path {
        let branch = g.unions[u].swap_remove(b);
        g.unions[u] = vec![branch];
        isolated.push((u, 0));
        g = &mut g.unions[u][0];
    }
    isolated
}

/// Makes the patterns of `group`, and those in it, without their own
/// `as_of` see the graph as of `as_of`.
fn set_as_of(group: &mut Group, as_of: u64) {
    for p in &mut group.patterns {
        p.as_of.get_or_insert(as_of);
    }
    for g in group.unions.iter_mut().flatten().chain(&mut group.nots) {
        set_as_of(g, as_of);
    }
}

/// The variables `group` uses, apart from in `skip`.
fn vars_except(group: &Group, skip: *const Group, out: &mut BTreeSet<Var>) {
    if std::ptr::eq(group, skip) {
        return;
    }
    out.extend(group.patterns.iter().flat_map(Pattern::vars));
    out.extend(group.filters.iter().flat_map(|f| f.vars()));
    for g in group.unions.iter().flatten().chain(&group.nots) {
        vars_except(g, skip, out);
    }
}

/// Removes from `group`, and those in it, the filters and negations using
/// any of `outer`, so that its rows only grow.
fn strip(group: &mut Group, outer: &BTreeSet<Var>) {
    group
        .filters
        .retain(|f| f.vars().iter().all(|v| !outer.contains(v)));
    group.nots.retain(|n| n.all_vars().is_disjoint(outer));
    for g in group.unions.iter_mut().flatten() {
        strip(g, outer);
    }
}

/// Whether `link` can match `p`'s constants.
fn fits(p: &Pattern, &(_, spo): &Link) -> bool {
    p.terms()
        .iter()
        .zip(spo)
        .all(|(t, v)| !matches!(t, Term::Id(c) if *c != v))
}

fn run(q: &Query, g: &Graph, overrides: &Overrides) -> Result<Vec<Vec<u64>>> {
    plan::plan_full(q, g, None, overrides, None)?
        .execute(g)
        .collect()
}

/// The changes to the selected rows of `q`, which has no `as_of`, for
/// `delta`: how many times each row is added (or removed, if negative).
fn row_changes(q: &Query, g: &Graph, delta: &Delta) -> Result<HashMap<Vec<u64>, i64>> {
    let mut all = Vec::new();
    args(&q.root, &mut Vec::new(), &mut all);
    let mut changes: HashMap<Vec<u64>, i64> = HashMap::new();
    for (i, arg) in all.iter().enumerate() {
        // The other parts as of now before this one, and then after it.
        let mut base = q.clone();
        for (j, other) in all.iter().enumerate() {
            let as_of = if j < i { delta.new } else { delta.old };
            match other {
                Arg::Pattern(path, k) => {
                    group_at_mut(&mut base.root, path).patterns[*k].as_of = Some(as_of);
                }
                Arg::Not(path, k) => {
                    set_as_of(&mut group_at_mut(&mut base.root, path).nots[*k], as_of)
                }
            }
        }
        // A union adds its branches, so only the branch with this part
        // changes with it.
        let isolated = match arg {
            Arg::Pattern(path, _) | Arg::Not(path, _) => isolate(&mut base.root, path),
        };
        match arg {
            Arg::Pattern(path, k) => {
                let pattern = group_at(&q.root, path).patterns[*k];
                for (sign, links) in [(1, &delta.added), (-1, &delta.removed)] {
                    let links: Vec<Link> = links
                        .iter()
                        .copied()
                        .filter(|l| fits(&pattern, l))
                        .collect();
                    if links.is_empty() {
                        continue;
                    }
                    let address = group_at(&base.root, &isolated) as *const Group as usize;
                    let mut overrides = Overrides::default();
                    overrides.patterns.insert((address, *k), links);
                    for row in run(&base, g, &overrides)? {
                        *changes.entry(row).or_default() += sign;
                    }
                }
            }
            Arg::Not(path, k) => {
                let enclosing = group_at(&q.root, path);
                let not = &enclosing.nots[*k];
                // The variables of the rows the negation's changes are for.
                let flip_vars: Vec<Var> = not
                    .bound_vars()
                    .intersection(&enclosing.bound_vars())
                    .copied()
                    .collect();
                // The negation alone, without what it needs from outside.
                let mut outer = BTreeSet::new();
                vars_except(&q.root, not, &mut outer);
                for v in not.bound_vars() {
                    outer.remove(&v);
                }
                let mut alone = Query {
                    names: q.names.clone(),
                    root: not.clone(),
                    select: Some(flip_vars.clone()),
                    ..Query::default()
                };
                strip(&mut alone.root, &outer);
                let flips: HashSet<Vec<u64>> = row_changes(&alone, g, delta)?
                    .into_iter()
                    .filter(|&(_, n)| n != 0)
                    .map(|(row, _)| row)
                    .collect();
                if flips.is_empty() {
                    continue;
                }
                let flips = Arc::new(flips);
                let (mut old, mut new) = (not.clone(), not.clone());
                set_as_of(&mut old, delta.old);
                set_as_of(&mut new, delta.new);
                for (sign, to_unmatched) in [(1, true), (-1, false)] {
                    let address = group_at(&base.root, &isolated) as *const Group as usize;
                    let mut overrides = Overrides::default();
                    overrides.nots.insert(
                        (address, *k),
                        NotChange {
                            old: old.clone(),
                            new: new.clone(),
                            flips: flips.clone(),
                            flip_vars: flip_vars.clone(),
                            to_unmatched,
                        },
                    );
                    for row in run(&base, g, &overrides)? {
                        *changes.entry(row).or_default() += sign;
                    }
                }
            }
        }
    }
    Ok(changes)
}
