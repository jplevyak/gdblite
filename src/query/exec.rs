//! Running a [`Plan`]: pull-based operators passing batches of rows.
//!
//! A row has a slot for each variable of the query, of which those bound so
//! far are meaningful.  Each operator fills a batch with up to
//! [`BATCH_ROWS`] rows on request, and an empty batch means it is done.

use std::cmp::Ordering;
use std::collections::{BTreeSet, HashMap, HashSet};

use super::plan::{Access, GroupPlan, Join, Plan, Step};
use super::{Filter, Pattern, SortKey, Term, Var};
use crate::graph::{Graph, Result};
use crate::local_table::IdSet;
use crate::record::Linkage;
use crate::tuple_index::{self, Intersection};

/// The most rows in a batch.
const BATCH_ROWS: usize = 1024;

/// Rows of `width` ids each.
#[derive(Debug, Clone)]
struct Batch {
    width: usize,
    data: Vec<u64>,
}

impl Batch {
    fn new(width: usize) -> Self {
        Batch {
            width,
            data: Vec::new(),
        }
    }

    fn len(&self) -> usize {
        self.data.len() / self.width
    }

    fn is_empty(&self) -> bool {
        self.data.is_empty()
    }

    fn is_full(&self) -> bool {
        self.len() >= BATCH_ROWS
    }

    fn row(&self, i: usize) -> &[u64] {
        &self.data[i * self.width..(i + 1) * self.width]
    }

    fn push(&mut self, row: &[u64]) {
        self.data.extend_from_slice(row);
    }

    fn clear(&mut self) {
        self.data.clear();
    }
}

trait Operator {
    /// Fills `out`, which is empty, with the next rows, leaving it empty if
    /// there are no more.
    fn next(&mut self, g: &Graph, out: &mut Batch) -> Result<()>;
}

/// Runs `op` to the end, appending its rows to `out`.
fn drain(op: &mut dyn Operator, g: &Graph, out: &mut Batch) -> Result<()> {
    let mut batch = Batch::new(out.width);
    loop {
        batch.clear();
        op.next(g, &mut batch)?;
        if batch.is_empty() {
            return Ok(());
        }
        out.data.extend_from_slice(&batch.data);
    }
}

/// Given rows, once.
struct Seed {
    rows: Option<Batch>,
}

impl Operator for Seed {
    fn next(&mut self, _: &Graph, out: &mut Batch) -> Result<()> {
        if let Some(rows) = self.rows.take() {
            out.data = rows.data;
        }
        Ok(())
    }
}

/// A single row binding nothing.
fn unit(width: usize) -> Box<dyn Operator> {
    let mut row = Batch::new(width);
    row.data.resize(width, 0);
    Box::new(Seed { rows: Some(row) })
}

/// The rows of each operator in turn.
struct Concat {
    ops: Vec<Box<dyn Operator>>,
}

impl Operator for Concat {
    fn next(&mut self, g: &Graph, out: &mut Batch) -> Result<()> {
        while let Some(op) = self.ops.first_mut() {
            op.next(g, out)?;
            if !out.is_empty() {
                return Ok(());
            }
            self.ops.remove(0);
        }
        Ok(())
    }
}

/// Finds the live links matching a pattern, given a row.
struct Matcher {
    pattern: Pattern,
    access: Access,
    /// Whether each variable is bound in the rows given.
    bound: Vec<bool>,
    as_of: Option<u64>,
}

impl Matcher {
    fn value(&self, t: Term, row: &[u64]) -> Option<u64> {
        match t {
            Term::Id(id) => Some(id),
            Term::Var(v) if self.bound[v.0] => Some(row[v.0]),
            Term::Var(_) => None,
        }
    }

    /// The candidate links for `row`: a superset of the matches, possibly
    /// including dead links and ids that are not links.
    fn candidates(&self, g: &Graph, row: &[u64]) -> IdSet {
        let local = g.local();
        let p = &self.pattern;
        let [s, pr, o] = p.terms().map(|t| self.value(t, row));
        let v = |x: Option<u64>| x.expect("bound by the access");
        match self.access {
            Access::Link => {
                let link = row[p.link.expect("a link variable").0];
                match g
                    .get(link)
                    .ok()
                    .and_then(|r| r.as_link().map(|l| l.fields()))
                {
                    Some(f) => {
                        tuple_index::find_links_by_spo(local, f.subject, f.predicate, f.object)
                    }
                    None => IdSet::new(),
                }
            }
            Access::Spo => tuple_index::find_links_by_spo(local, v(s), v(pr), v(o)),
            Access::SubjectPredicate => tuple_index::find_links_by_intersection(
                local,
                Intersection::SubjectPredicate,
                v(s),
                v(pr),
            ),
            Access::PredicateObject => tuple_index::find_links_by_intersection(
                local,
                Intersection::PredicateObject,
                v(pr),
                v(o),
            ),
            Access::Subject => tuple_index::find_links(local, Linkage::Subject, v(s)),
            Access::Predicate => tuple_index::find_links(local, Linkage::Predicate, v(pr)),
            Access::Object => tuple_index::find_links(local, Linkage::Object, v(o)),
            Access::All => {
                let predicates: BTreeSet<u64> = g.stats().predicates().map(|(p, _)| p).collect();
                let mut links = IdSet::new();
                for p in predicates {
                    links.extend(tuple_index::find_links(local, Linkage::Predicate, p));
                }
                links
            }
        }
    }

    /// Appends to `out` a row extending `row` for each live link matching.
    fn matches(&self, g: &Graph, row: &[u64], out: &mut Batch) -> Result<()> {
        let candidates = self.candidates(g, row);
        // The latest link of each subject, predicate and object.
        let mut latest: HashMap<[u64; 3], (u64, bool)> = HashMap::new();
        for &id in candidates.range(..=self.as_of.unwrap_or(u64::MAX)) {
            let Ok(record) = g.get(id) else { continue };
            if let Some(link) = record.as_link() {
                let spo = [link.subject(), link.predicate(), link.object()];
                latest.insert(spo, (id, link.dead()));
            }
        }
        let p = &self.pattern;
        let mut live: Vec<(u64, [u64; 3])> = latest
            .into_iter()
            .filter(|&(_, (_, dead))| !dead)
            .map(|(spo, (id, _))| (id, spo))
            .collect();
        live.sort_unstable();
        let mut new = row.to_vec();
        'links: for (id, [s, pr, o]) in live {
            new.copy_from_slice(row);
            // The variables set from this link, which a repeat must match.
            let mut set = Vec::new();
            let fields = [
                (Some(p.subject), s),
                (Some(p.predicate), pr),
                (Some(p.object), o),
                (p.link.map(Term::Var), id),
            ];
            for (term, value) in fields {
                match term {
                    None => {}
                    Some(Term::Id(c)) => {
                        if c != value {
                            continue 'links;
                        }
                    }
                    Some(Term::Var(v)) if self.bound[v.0] || set.contains(&v) => {
                        if new[v.0] != value {
                            continue 'links;
                        }
                    }
                    Some(Term::Var(v)) => {
                        new[v.0] = value;
                        set.push(v);
                    }
                }
            }
            out.push(&new);
        }
        Ok(())
    }
}

/// Rows extended by each matching link of a pattern, looked up per row.
struct BindJoin {
    input: Box<dyn Operator>,
    matcher: Matcher,
    in_batch: Batch,
    in_pos: usize,
    pending: Batch,
    pending_pos: usize,
}

impl Operator for BindJoin {
    fn next(&mut self, g: &Graph, out: &mut Batch) -> Result<()> {
        loop {
            while self.pending_pos < self.pending.len() {
                if out.is_full() {
                    return Ok(());
                }
                out.push(self.pending.row(self.pending_pos));
                self.pending_pos += 1;
            }
            if out.is_full() {
                return Ok(());
            }
            if self.in_pos >= self.in_batch.len() {
                self.in_batch.clear();
                self.in_pos = 0;
                self.input.next(g, &mut self.in_batch)?;
                if self.in_batch.is_empty() {
                    return Ok(());
                }
            }
            self.pending.clear();
            self.pending_pos = 0;
            let row = self.in_batch.row(self.in_pos);
            self.matcher.matches(g, row, &mut self.pending)?;
            self.in_pos += 1;
        }
    }
}

/// Rows joined on `keys` to the rows of another operator, read once into a
/// hash table.
struct HashJoin {
    probe: Box<dyn Operator>,
    /// The other rows, until the table is built.
    build: Option<Box<dyn Operator>>,
    keys: Vec<Var>,
    /// The variables the other rows bind that the probe rows do not.
    new_vars: Vec<Var>,
    /// The values of `new_vars` of each other row, by its key values; with
    /// no new variables, a 0 for each row.
    table: HashMap<Vec<u64>, Vec<u64>>,
    in_batch: Batch,
    in_pos: usize,
    /// The matches of the current probe row: its index and the next value.
    current: Option<(usize, usize)>,
}

impl HashJoin {
    fn key(&self, row: &[u64]) -> Vec<u64> {
        self.keys.iter().map(|v| row[v.0]).collect()
    }

    fn build_table(&mut self, g: &Graph) -> Result<()> {
        let Some(mut build) = self.build.take() else {
            return Ok(());
        };
        let mut rows = Batch::new(self.in_batch.width);
        drain(build.as_mut(), g, &mut rows)?;
        for i in 0..rows.len() {
            let row = rows.row(i);
            let values = self.table.entry(self.key(row)).or_default();
            if self.new_vars.is_empty() {
                values.push(0);
            }
            values.extend(self.new_vars.iter().map(|v| row[v.0]));
        }
        Ok(())
    }
}

impl Operator for HashJoin {
    fn next(&mut self, g: &Graph, out: &mut Batch) -> Result<()> {
        self.build_table(g)?;
        let stride = self.new_vars.len().max(1);
        loop {
            if let Some((i, mut pos)) = self.current {
                let mut new = self.in_batch.row(i).to_vec();
                let key = self.key(&new);
                let values = &self.table[&key];
                while pos < values.len() {
                    if out.is_full() {
                        self.current = Some((i, pos));
                        return Ok(());
                    }
                    for (k, v) in self.new_vars.iter().enumerate() {
                        new[v.0] = values[pos + k];
                    }
                    out.push(&new);
                    pos += stride;
                }
                self.current = None;
            }
            if out.is_full() {
                return Ok(());
            }
            if self.in_pos >= self.in_batch.len() {
                self.in_batch.clear();
                self.in_pos = 0;
                self.probe.next(g, &mut self.in_batch)?;
                if self.in_batch.is_empty() {
                    return Ok(());
                }
            }
            let i = self.in_pos;
            self.in_pos += 1;
            if self.table.contains_key(&self.key(self.in_batch.row(i))) {
                self.current = Some((i, 0));
            }
        }
    }
}

/// The rows of a union's branches, evaluated for each batch of input rows.
struct UnionBind {
    input: Box<dyn Operator>,
    branches: Vec<GroupPlan>,
    bound: Vec<bool>,
    ctx: Context,
    pending: Batch,
    pending_pos: usize,
}

impl Operator for UnionBind {
    fn next(&mut self, g: &Graph, out: &mut Batch) -> Result<()> {
        loop {
            while self.pending_pos < self.pending.len() {
                if out.is_full() {
                    return Ok(());
                }
                out.push(self.pending.row(self.pending_pos));
                self.pending_pos += 1;
            }
            let mut rows = Batch::new(out.width);
            self.input.next(g, &mut rows)?;
            if rows.is_empty() {
                return Ok(());
            }
            self.pending.clear();
            self.pending_pos = 0;
            for branch in &self.branches {
                let seed = Box::new(Seed {
                    rows: Some(rows.clone()),
                });
                let mut op = self.ctx.build(branch, seed, &mut self.bound.clone());
                drain(op.as_mut(), g, &mut self.pending)?;
            }
        }
    }
}

/// The rows for which a group has no match, given their values of `keys`.
/// For each batch, the group is evaluated for its rows; or, if `once` is
/// set, it is evaluated once alone.
struct AntiJoin {
    input: Box<dyn Operator>,
    group: GroupPlan,
    keys: Vec<Var>,
    bound: Vec<bool>,
    ctx: Context,
    /// For evaluating once: the keys with a match, once known.
    once: Option<Option<HashSet<Vec<u64>>>>,
}

impl AntiJoin {
    /// The keys of the rows the group has a match for, evaluated for `rows`
    /// (or alone, if `rows` is `None`).
    fn matched(&self, g: &Graph, rows: Option<&Batch>) -> Result<HashSet<Vec<u64>>> {
        let (seed, mut bound): (Box<dyn Operator>, Vec<bool>) = match rows {
            Some(rows) => (
                Box::new(Seed {
                    rows: Some(rows.clone()),
                }),
                self.bound.clone(),
            ),
            None => (unit(self.ctx.width), vec![false; self.bound.len()]),
        };
        let mut op = self.ctx.build(&self.group, seed, &mut bound);
        let mut matches = Batch::new(self.ctx.width);
        drain(op.as_mut(), g, &mut matches)?;
        Ok((0..matches.len())
            .map(|i| self.keys.iter().map(|v| matches.row(i)[v.0]).collect())
            .collect())
    }
}

impl Operator for AntiJoin {
    fn next(&mut self, g: &Graph, out: &mut Batch) -> Result<()> {
        if let Some(None) = self.once {
            self.once = Some(Some(self.matched(g, None)?));
        }
        loop {
            let mut rows = Batch::new(out.width);
            self.input.next(g, &mut rows)?;
            if rows.is_empty() {
                return Ok(());
            }
            let fresh;
            let matched = match &self.once {
                Some(Some(m)) => m,
                _ => {
                    fresh = self.matched(g, Some(&rows))?;
                    &fresh
                }
            };
            for i in 0..rows.len() {
                let row = rows.row(i);
                let key: Vec<u64> = self.keys.iter().map(|v| row[v.0]).collect();
                if !matched.contains(&key) {
                    out.push(row);
                }
            }
            if !out.is_empty() {
                return Ok(());
            }
        }
    }
}

/// The rows passing every filter.
struct FilterOp {
    input: Box<dyn Operator>,
    filters: Vec<Filter>,
    batch: Batch,
}

impl Operator for FilterOp {
    fn next(&mut self, g: &Graph, out: &mut Batch) -> Result<()> {
        loop {
            self.batch.clear();
            self.input.next(g, &mut self.batch)?;
            if self.batch.is_empty() {
                return Ok(());
            }
            for i in 0..self.batch.len() {
                let row = self.batch.row(i);
                if self.filters.iter().all(|f| f.eval(g, row)) {
                    out.push(row);
                }
            }
            if !out.is_empty() {
                return Ok(());
            }
        }
    }
}

/// The first row with each value of `keys`.
struct Distinct {
    input: Box<dyn Operator>,
    keys: Vec<Var>,
    seen: HashSet<Vec<u64>>,
    batch: Batch,
}

impl Operator for Distinct {
    fn next(&mut self, g: &Graph, out: &mut Batch) -> Result<()> {
        loop {
            self.batch.clear();
            self.input.next(g, &mut self.batch)?;
            if self.batch.is_empty() {
                return Ok(());
            }
            for i in 0..self.batch.len() {
                let row = self.batch.row(i);
                if self
                    .seen
                    .insert(self.keys.iter().map(|v| row[v.0]).collect())
                {
                    out.push(row);
                }
            }
            if !out.is_empty() {
                return Ok(());
            }
        }
    }
}

/// All the rows, in order.
struct Sort {
    input: Option<Box<dyn Operator>>,
    keys: Vec<SortKey>,
    rows: Batch,
    pos: usize,
}

impl Sort {
    fn compare(g: &Graph, keys: &[SortKey], a: &[u64], b: &[u64]) -> Ordering {
        for k in keys {
            let (x, y) = (a[k.var.0], b[k.var.0]);
            let mut ord = if k.by_data {
                g.data(x).cmp(&g.data(y)).then(x.cmp(&y))
            } else {
                x.cmp(&y)
            };
            if k.descending {
                ord = ord.reverse();
            }
            if ord != Ordering::Equal {
                return ord;
            }
        }
        Ordering::Equal
    }
}

impl Operator for Sort {
    fn next(&mut self, g: &Graph, out: &mut Batch) -> Result<()> {
        if let Some(mut input) = self.input.take() {
            drain(input.as_mut(), g, &mut self.rows)?;
            let width = self.rows.width;
            let mut order: Vec<usize> = (0..self.rows.len()).collect();
            order
                .sort_by(|&i, &j| Self::compare(g, &self.keys, self.rows.row(i), self.rows.row(j)));
            let mut sorted = Vec::with_capacity(self.rows.data.len());
            for i in order {
                sorted.extend_from_slice(&self.rows.data[i * width..(i + 1) * width]);
            }
            self.rows.data = sorted;
        }
        while self.pos < self.rows.len() && !out.is_full() {
            out.push(self.rows.row(self.pos));
            self.pos += 1;
        }
        Ok(())
    }
}

/// The first rows.
struct Limit {
    input: Box<dyn Operator>,
    remaining: usize,
}

impl Operator for Limit {
    fn next(&mut self, g: &Graph, out: &mut Batch) -> Result<()> {
        if self.remaining == 0 {
            return Ok(());
        }
        self.input.next(g, out)?;
        let keep = out.len().min(self.remaining);
        out.data.truncate(keep * out.width);
        self.remaining -= keep;
        Ok(())
    }
}

/// What building operators needs to know.
#[derive(Debug, Clone, Copy)]
struct Context {
    /// The slots of a row.
    width: usize,
    /// The query's `as_of`.
    as_of: Option<u64>,
}

impl Context {
    /// The operator evaluating `plan` for the rows of `input`, which bind
    /// `bound`; on return, `bound` is what its rows bind.
    fn build(
        &self,
        plan: &GroupPlan,
        input: Box<dyn Operator>,
        bound: &mut [bool],
    ) -> Box<dyn Operator> {
        let width = self.width;
        let mut op = input;
        for step in &plan.steps {
            op = match step {
                Step::Pattern {
                    pattern,
                    join,
                    access,
                    keys,
                    ..
                } => {
                    let scan = |input, bound: &[bool]| BindJoin {
                        input,
                        matcher: Matcher {
                            pattern: *pattern,
                            access: *access,
                            bound: bound.to_vec(),
                            as_of: pattern.as_of.or(self.as_of),
                        },
                        in_batch: Batch::new(width),
                        in_pos: 0,
                        pending: Batch::new(width),
                        pending_pos: 0,
                    };
                    let joined: Box<dyn Operator> = match join {
                        Join::Bind => Box::new(scan(op, bound)),
                        Join::Hash => Box::new(HashJoin {
                            probe: op,
                            build: Some(Box::new(scan(unit(width), &vec![false; width]))),
                            keys: keys.clone(),
                            new_vars: new_vars(pattern.vars(), bound),
                            table: HashMap::new(),
                            in_batch: Batch::new(width),
                            in_pos: 0,
                            current: None,
                        }),
                    };
                    pattern.vars().for_each(|v| bound[v.0] = true);
                    joined
                }
                Step::Union {
                    branches,
                    vars,
                    keys,
                    join,
                    ..
                } => {
                    let joined: Box<dyn Operator> = match join {
                        Join::Bind => Box::new(UnionBind {
                            input: op,
                            branches: branches.clone(),
                            bound: bound.to_vec(),
                            ctx: *self,
                            pending: Batch::new(width),
                            pending_pos: 0,
                        }),
                        Join::Hash => {
                            let ops = branches
                                .iter()
                                .map(|b| self.build(b, unit(width), &mut vec![false; width]))
                                .collect();
                            Box::new(HashJoin {
                                probe: op,
                                build: Some(Box::new(Concat { ops })),
                                keys: keys.clone(),
                                new_vars: new_vars(vars.iter().copied(), bound),
                                table: HashMap::new(),
                                in_batch: Batch::new(width),
                                in_pos: 0,
                                current: None,
                            })
                        }
                    };
                    vars.iter().for_each(|v| bound[v.0] = true);
                    joined
                }
                Step::Filter(filters) => Box::new(FilterOp {
                    input: op,
                    filters: filters.clone(),
                    batch: Batch::new(width),
                }),
                Step::Not {
                    group, keys, join, ..
                } => Box::new(AntiJoin {
                    input: op,
                    group: group.clone(),
                    keys: keys.clone(),
                    bound: bound.to_vec(),
                    ctx: *self,
                    once: (*join == Join::Hash).then_some(None),
                }),
            };
        }
        op
    }
}

/// The distinct variables of `vars` that are not `bound`.
fn new_vars(vars: impl Iterator<Item = Var>, bound: &[bool]) -> Vec<Var> {
    let mut vars: Vec<Var> = vars.filter(|v| !bound[v.0]).collect();
    vars.sort();
    vars.dedup();
    vars
}

/// The rows of a query, made by [`Plan::execute`].
pub struct Rows<'g> {
    g: &'g Graph,
    op: Box<dyn Operator>,
    select: Vec<Var>,
    batch: Batch,
    pos: usize,
    done: bool,
}

impl Iterator for Rows<'_> {
    type Item = Result<Vec<u64>>;

    fn next(&mut self) -> Option<Self::Item> {
        while self.pos >= self.batch.len() {
            if self.done {
                return None;
            }
            self.batch.clear();
            self.pos = 0;
            if let Err(e) = self.op.next(self.g, &mut self.batch) {
                self.done = true;
                return Some(Err(e));
            }
            if self.batch.is_empty() {
                self.done = true;
                return None;
            }
        }
        let row = self.batch.row(self.pos);
        self.pos += 1;
        Some(Ok(self.select.iter().map(|v| row[v.0]).collect()))
    }
}

impl Plan {
    /// Runs the plan on `g`, which should be the graph it was planned for
    /// (otherwise the results are right but the plan may be slow).
    pub fn execute<'g>(&self, g: &'g Graph) -> Rows<'g> {
        let q = &self.query;
        // A row has a slot even with no variables, so that it is a row.
        let ctx = Context {
            width: q.names.len().max(1),
            as_of: q.as_of,
        };
        let mut op = ctx.build(&self.root, unit(ctx.width), &mut vec![false; ctx.width]);
        let select = q.selected();
        if q.distinct {
            op = Box::new(Distinct {
                input: op,
                keys: select.clone(),
                seen: HashSet::new(),
                batch: Batch::new(ctx.width),
            });
        }
        if !q.order.is_empty() {
            op = Box::new(Sort {
                input: Some(op),
                keys: q.order.clone(),
                rows: Batch::new(ctx.width),
                pos: 0,
            });
        }
        if let Some(n) = q.limit {
            op = Box::new(Limit {
                input: op,
                remaining: n,
            });
        }
        Rows {
            g,
            op,
            select,
            batch: Batch::new(ctx.width),
            pos: 0,
            done: false,
        }
    }
}
