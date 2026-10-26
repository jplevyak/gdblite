//! Running a [`Plan`]: pull-based operators passing batches of rows.
//!
//! A row has a slot for each variable of the query, of which those bound so
//! far are meaningful.  Each operator fills a batch with up to
//! [`BATCH_ROWS`] rows on request, and an empty batch means it is done.

use std::collections::{BTreeSet, HashMap};

use super::plan::{Access, Join, Plan, shared_vars};
use super::{Filter, Pattern, Term, Var};
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

/// A single row binding nothing.
struct Unit {
    done: bool,
}

impl Operator for Unit {
    fn next(&mut self, _: &Graph, out: &mut Batch) -> Result<()> {
        if !self.done {
            self.done = true;
            out.data.resize(out.width, 0);
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

/// Rows joined on their shared variables to the rows of a pattern, scanned
/// once into a hash table.
struct HashJoin {
    probe: Box<dyn Operator>,
    /// The scan of the pattern, until the table is built.
    build: Option<Box<dyn Operator>>,
    keys: Vec<Var>,
    /// The variables the pattern binds that the probe rows do not.
    new_vars: Vec<Var>,
    /// The values of `new_vars` of each scanned row, by its key values.
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
        let mut batch = Batch::new(self.in_batch.width);
        loop {
            batch.clear();
            build.next(g, &mut batch)?;
            if batch.is_empty() {
                return Ok(());
            }
            for i in 0..batch.len() {
                let row = batch.row(i);
                let values = self.table.entry(self.key(row)).or_default();
                values.extend(self.new_vars.iter().map(|v| row[v.0]));
            }
        }
    }
}

impl Operator for HashJoin {
    fn next(&mut self, g: &Graph, out: &mut Batch) -> Result<()> {
        self.build_table(g)?;
        let n = self.new_vars.len();
        loop {
            if let Some((i, mut pos)) = self.current {
                let mut new = self.in_batch.row(i).to_vec();
                let key = self.key(&new);
                let values = &self.table[&key];
                // With no new variables, a matching key is one match.
                while pos < values.len() || (n == 0 && pos == 0) {
                    if out.is_full() {
                        self.current = Some((i, pos));
                        return Ok(());
                    }
                    for (k, v) in self.new_vars.iter().enumerate() {
                        new[v.0] = values[pos + k];
                    }
                    out.push(&new);
                    pos += n.max(1);
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
        let width = q.names.len().max(1);
        let filters = |ids: &[usize]| ids.iter().map(|&f| q.filters[f].clone()).collect();
        let with_filters = |op: Box<dyn Operator>, ids: &[usize]| -> Box<dyn Operator> {
            if ids.is_empty() {
                op
            } else {
                Box::new(FilterOp {
                    input: op,
                    filters: filters(ids),
                    batch: Batch::new(width),
                })
            }
        };
        let unit = || Box::new(Unit { done: false }) as Box<dyn Operator>;
        let mut op = with_filters(unit(), &self.initial_filters);
        let mut bound = vec![false; width];
        for step in &self.steps {
            let pattern = q.patterns[step.pattern];
            op = match step.join {
                Join::Bind => Box::new(BindJoin {
                    input: op,
                    matcher: Matcher {
                        pattern,
                        access: step.access,
                        bound: bound.clone(),
                        as_of: q.as_of,
                    },
                    in_batch: Batch::new(width),
                    in_pos: 0,
                    pending: Batch::new(width),
                    pending_pos: 0,
                }),
                Join::Hash => {
                    let scan = BindJoin {
                        input: unit(),
                        matcher: Matcher {
                            pattern,
                            access: step.access,
                            bound: vec![false; width],
                            as_of: q.as_of,
                        },
                        in_batch: Batch::new(width),
                        in_pos: 0,
                        pending: Batch::new(width),
                        pending_pos: 0,
                    };
                    let keys = shared_vars(&pattern, &bound);
                    let mut new_vars: Vec<Var> = pattern.vars().filter(|v| !bound[v.0]).collect();
                    new_vars.sort();
                    new_vars.dedup();
                    Box::new(HashJoin {
                        probe: op,
                        build: Some(Box::new(scan)),
                        keys,
                        new_vars,
                        table: HashMap::new(),
                        in_batch: Batch::new(width),
                        in_pos: 0,
                        current: None,
                    })
                }
            };
            op = with_filters(op, &step.filters);
            pattern.vars().for_each(|v| bound[v.0] = true);
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
            select: q.selected(),
            batch: Batch::new(width),
            pos: 0,
            done: false,
        }
    }
}
