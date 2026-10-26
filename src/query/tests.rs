use std::cmp::Ordering;
use std::collections::HashMap;
use std::sync::Arc;

use super::plan::{self, Access, Join, Step};
use super::*;
use crate::graph::Options;
use crate::record::Linkage;

fn graph() -> Graph {
    Graph::open(None, Options::default()).unwrap()
}

/// A small deterministic random number generator (xorshift64*).
struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }

    fn below(&mut self, n: usize) -> usize {
        (self.next() % n as u64) as usize
    }

    fn chance(&mut self, percent: u64) -> bool {
        self.next() % 100 < percent
    }

    fn pick<T: Copy>(&mut self, items: &[T]) -> T {
        items[self.below(items.len())]
    }
}

/// Every live link (as of `as_of`): its id, subject, predicate and object.
fn live_links(g: &Graph, as_of: Option<u64>) -> Vec<(u64, [u64; 3])> {
    let predicates: Vec<u64> = g.stats().predicates().map(|(p, _)| p).collect();
    let mut links = Vec::new();
    for p in predicates {
        for id in g.live(&g.links_at(Linkage::Predicate, [p]), as_of) {
            let l = g.get(id).unwrap().as_link().unwrap().fields();
            links.push((id, [l.subject, l.predicate, l.object]));
        }
    }
    links
}

/// Evaluates queries by trying every combination of links.
struct BruteForce<'a> {
    q: &'a Query,
    g: &'a Graph,
    links: HashMap<Option<u64>, Vec<(u64, [u64; 3])>>,
}

type Partial = Vec<Option<u64>>;

impl BruteForce<'_> {
    fn links(&mut self, as_of: Option<u64>) -> Vec<(u64, [u64; 3])> {
        let g = self.g;
        self.links
            .entry(as_of)
            .or_insert_with(|| live_links(g, as_of))
            .clone()
    }

    fn full(row: &Partial) -> Vec<u64> {
        row.iter().map(|v| v.unwrap_or(0)).collect()
    }

    fn group(&mut self, group: &Group, row: &Partial) -> Vec<Partial> {
        let mut rows = vec![row.clone()];
        for p in &group.patterns {
            let links = self.links(p.as_of.or(self.q.as_of));
            let mut next = Vec::new();
            for row in &rows {
                for &(id, spo) in &links {
                    let mut new = row.clone();
                    let fields = [
                        (Some(p.subject), spo[0]),
                        (Some(p.predicate), spo[1]),
                        (Some(p.object), spo[2]),
                        (p.link.map(Term::Var), id),
                    ];
                    let ok = fields.iter().all(|&(term, value)| match term {
                        None => true,
                        Some(Term::Id(c)) => c == value,
                        Some(Term::Var(v)) => match new[v.0] {
                            Some(bound) => bound == value,
                            None => {
                                new[v.0] = Some(value);
                                true
                            }
                        },
                    });
                    if ok {
                        next.push(new);
                    }
                }
            }
            rows = next;
        }
        for branches in &group.unions {
            let mut next = Vec::new();
            for row in &rows {
                for b in branches {
                    next.extend(self.group(b, row));
                }
            }
            rows = next;
        }
        let g = self.g;
        rows.retain(|r| group.filters.iter().all(|f| f.eval(g, &Self::full(r))));
        rows.retain(|r| group.nots.iter().all(|n| self.group(n, r).is_empty()));
        rows
    }

    /// The query's rows, selected and made distinct, but not ordered or
    /// limited.
    fn rows(q: &Query, g: &Graph) -> Vec<Vec<u64>> {
        let mut bf = BruteForce {
            q,
            g,
            links: HashMap::new(),
        };
        let rows = bf.group(&q.root, &vec![None; q.names.len()]);
        let mut out: Vec<Vec<u64>> = rows
            .iter()
            .map(|r| q.selected().iter().map(|v| r[v.0].unwrap()).collect())
            .collect();
        if q.distinct {
            out.sort();
            out.dedup();
        }
        out
    }
}

fn sorted(mut rows: Vec<Vec<u64>>) -> Vec<Vec<u64>> {
    rows.sort();
    rows
}

/// Compares selected rows by the query's sort keys, which must be selected.
fn compare(q: &Query, g: &Graph, a: &[u64], b: &[u64]) -> Ordering {
    let selected = q.selected();
    for k in &q.order {
        let i = selected.iter().position(|&v| v == k.var).unwrap();
        let (x, y) = (a[i], b[i]);
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

/// Checks `rows` of `q` against `expected`, all of its rows unordered and
/// without the limit.
fn check_rows(q: &Query, g: &Graph, rows: Vec<Vec<u64>>, expected: &[Vec<u64>], what: &str) {
    let ordered = |rows: &[Vec<u64>]| {
        rows.windows(2)
            .all(|w| compare(q, g, &w[0], &w[1]) != Ordering::Greater)
    };
    assert!(ordered(&rows), "not ordered: {q:?}\n{what}");
    match q.limit {
        None => assert_eq!(sorted(rows), sorted(expected.to_vec()), "{q:?}\n{what}"),
        Some(n) => {
            assert_eq!(rows.len(), n.min(expected.len()), "{q:?}\n{what}");
            for r in &rows {
                assert!(expected.contains(r), "{q:?}\n{what}");
            }
            if !q.order.is_empty() {
                // The rows are the first in order, up to ties.
                let mut all = expected.to_vec();
                all.sort_by(|a, b| compare(q, g, a, b));
                if let (Some(last), Some(next)) = (rows.last(), all.get(rows.len())) {
                    assert_ne!(compare(q, g, last, next), Ordering::Greater, "{what}");
                }
            }
        }
    }
}

/// Checks `q` against brute force, as planned and with each join used
/// wherever it can be.
fn check(q: &Query, g: &Graph) {
    let expected = BruteForce::rows(q, g);
    for force in [None, Some(Join::Bind), Some(Join::Hash)] {
        let plan = plan::plan_with(q, g, force).unwrap();
        let rows: Vec<Vec<u64>> = plan.execute(g).collect::<Result<_>>().unwrap();
        check_rows(q, g, rows, &expected, &plan.explain());
    }
}

/// A random graph: nodes, predicates, and links among them and about links.
fn random_graph(rng: &mut Rng) -> (Graph, Vec<u64>, Vec<u64>) {
    let mut g = graph();
    let nodes: Vec<u64> = (0..7).map(|_| g.node().unwrap()).collect();
    let predicates: Vec<u64> = (0..4)
        .map(|i| g.value(format!("p{i}").as_bytes()).unwrap())
        .collect();
    let mut links = Vec::new();
    for _ in 0..150 {
        let s = if rng.chance(10) && !links.is_empty() {
            rng.pick(&links)
        } else {
            rng.pick(&nodes)
        };
        let (p, o) = (rng.pick(&predicates), rng.pick(&nodes));
        if rng.chance(20) {
            g.delete_link(s, p, o).unwrap();
        } else {
            links.push(g.link(s, p, o).unwrap());
        }
    }
    (g, nodes, predicates)
}

/// Makes random queries over a graph.
struct Generator<'a> {
    rng: &'a mut Rng,
    nodes: &'a [u64],
    predicates: &'a [u64],
    ids: (u64, u64),
}

impl Generator<'_> {
    /// A term: one of `vars`, a new variable added to them, or a constant.
    fn term(&mut self, q: &mut Query, vars: &mut Vec<Var>, consts: &[u64], percent: u64) -> Term {
        if !self.rng.chance(percent) {
            return Term::Id(self.rng.pick(consts));
        }
        if vars.is_empty() || (vars.len() < 4 && self.rng.chance(40)) {
            vars.push(q.var(&format!("v{}", q.names.len())));
        }
        Term::Var(self.rng.pick(vars))
    }

    fn pattern(&mut self, q: &mut Query, group: &mut Group, vars: &mut Vec<Var>) {
        let (nodes, predicates) = (self.nodes, self.predicates);
        let s = self.term(q, vars, nodes, 70);
        let p = self.term(q, vars, predicates, 15);
        let o = self.term(q, vars, nodes, 70);
        group.pattern(s, p, o);
        if self.rng.chance(10) {
            group.patterns.last_mut().unwrap().link = Some(q.var("link"));
        }
        if self.rng.chance(10) {
            let (start, end) = self.ids;
            group.at(start + self.rng.next() % (end - start));
        }
    }

    /// A group binding exactly `vars`.
    fn branch(&mut self, q: &mut Query, vars: &[Var]) -> Group {
        let mut b = Group::new();
        for _ in 0..1 + self.rng.below(2) {
            let mut within = vars.to_vec();
            let (s, o) = (self.rng.pick(&within), self.rng.pick(&within));
            let s = if self.rng.chance(70) {
                Term::Var(s)
            } else {
                Term::Id(self.rng.pick(self.nodes))
            };
            let o = if self.rng.chance(70) {
                Term::Var(o)
            } else {
                Term::Id(self.rng.pick(self.nodes))
            };
            b.pattern(s, self.rng.pick(self.predicates), o);
            within.clear();
        }
        for &v in vars {
            if !b.bound_vars().contains(&v) {
                b.pattern(v, self.rng.pick(self.predicates), self.rng.pick(self.nodes));
            }
        }
        if vars.len() == 2 && self.rng.chance(30) {
            b.filter(Filter::Ne(vars[0].into(), vars[1].into()));
        }
        if self.rng.chance(20) {
            b.not(self.negation(q, vars));
        }
        b
    }

    /// A negated group using some of `outer` and its own variable.
    fn negation(&mut self, q: &mut Query, outer: &[Var]) -> Group {
        let mut n = Group::new();
        let mut vars: Vec<Var> = outer.to_vec();
        if self.rng.chance(40) || vars.is_empty() {
            vars.push(q.var("own"));
        }
        for _ in 0..1 + self.rng.below(2) {
            let s = self.term(q, &mut vars.clone(), self.nodes, 60);
            let o = self.term(q, &mut vars.clone(), self.nodes, 60);
            n.pattern(s, self.rng.pick(self.predicates), o);
        }
        if self.rng.chance(20) && !outer.is_empty() {
            n.filter(Filter::Ne(
                self.rng.pick(outer).into(),
                self.rng.pick(self.nodes).into(),
            ));
        }
        n
    }

    fn query(&mut self) -> Query {
        let mut q = Query::new();
        let mut root = Group::new();
        let mut vars = Vec::new();
        for _ in 0..1 + self.rng.below(3) {
            self.pattern(&mut q, &mut root, &mut vars);
        }
        if self.rng.chance(30) {
            let mut union_vars: Vec<Var> = Vec::new();
            for _ in 0..1 + self.rng.below(2) {
                if vars.is_empty() || self.rng.chance(40) {
                    vars.push(q.var("u"));
                }
                let v = self.rng.pick(&vars);
                if !union_vars.contains(&v) {
                    union_vars.push(v);
                }
            }
            let branches = (0..2)
                .map(|_| self.branch(&mut q, &union_vars))
                .collect::<Vec<_>>();
            root.union(branches);
        }
        if self.rng.chance(30) {
            let bound: Vec<Var> = root.bound_vars().into_iter().collect();
            root.not(self.negation(&mut q, &bound));
        }
        let bound: Vec<Var> = root.bound_vars().into_iter().collect();
        if bound.len() >= 2 && self.rng.chance(30) {
            let (a, b) = (self.rng.pick(&bound), self.rng.pick(&bound));
            root.filter(if self.rng.chance(50) {
                Filter::Ne(a.into(), b.into())
            } else {
                Filter::Lt(a.into(), b.into())
            });
        }
        q.root = root;
        if !bound.is_empty() && self.rng.chance(30) {
            let selected: Vec<Var> = bound
                .iter()
                .copied()
                .filter(|_| self.rng.chance(50))
                .collect();
            if !selected.is_empty() {
                q.select(selected).distinct();
            }
        }
        if self.rng.chance(30) {
            let selected = q.selected();
            if !selected.is_empty() {
                let key = SortKey::id(self.rng.pick(&selected));
                q.order_by(if self.rng.chance(50) { key.desc() } else { key });
            }
        }
        if self.rng.chance(20) {
            q.limit(self.rng.below(5));
        }
        if self.rng.chance(20) {
            let (start, end) = self.ids;
            q.as_of(start + self.rng.next() % (end - start));
        }
        q
    }
}

#[test]
fn random_queries() {
    let mut rng = Rng(0x9e37_79b9_7f4a_7c15);
    let (mut rows, mut hash) = (0, 0);
    for _ in 0..8 {
        let (g, nodes, predicates) = random_graph(&mut rng);
        let ids = (g.start_id(), g.next_id());
        for _ in 0..120 {
            let q = Generator {
                rng: &mut rng,
                nodes: &nodes,
                predicates: &predicates,
                ids,
            }
            .query();
            check(&q, &g);
            rows += usize::from(!BruteForce::rows(&q, &g).is_empty());
            let plan = q.plan(&g).unwrap();
            hash += usize::from(plan.explain().contains("hash join"));
        }
    }
    // The queries are not trivial.
    assert!(rows > 200, "{rows}");
    assert!(hash > 20, "{hash}");
}

fn first_step(plan: &Plan) -> (&Pattern, Access) {
    match &plan.root.steps[0] {
        Step::Pattern {
            pattern, access, ..
        } => (pattern, *access),
        step => panic!("{step:?}"),
    }
}

#[test]
fn chain_and_triangle() {
    let mut g = graph();
    let knows = g.value(b"knows").unwrap();
    let n: Vec<u64> = (0..6).map(|_| g.node().unwrap()).collect();
    for i in 0..6 {
        g.link(n[i], knows, n[(i + 1) % 6]).unwrap();
    }
    g.link(n[0], knows, n[2]).unwrap();
    g.link(n[2], knows, n[0]).unwrap();

    let mut q = Query::new();
    let (x, y) = (q.var("x"), q.var("y"));
    q.pattern(n[0], knows, x).pattern(x, knows, y).select([y]);
    assert_eq!(sorted(q.run(&g).unwrap()), [[n[0]], [n[2]], [n[3]]]);
    check(&q, &g);

    let mut q = Query::new();
    let (x, y, z) = (q.var("x"), q.var("y"), q.var("z"));
    q.pattern(x, knows, y)
        .pattern(y, knows, z)
        .pattern(z, knows, x)
        .filter(Filter::Lt(x.into(), y.into()))
        .filter(Filter::Lt(x.into(), z.into()));
    assert_eq!(q.run(&g).unwrap(), [[n[0], n[1], n[2]]]);
    check(&q, &g);
}

#[test]
fn planner_picks_selective_pattern_first() {
    let mut g = graph();
    let (kind, person, name) = (
        g.value(b"type").unwrap(),
        g.value(b"Person").unwrap(),
        g.value(b"name").unwrap(),
    );
    let mut bob = 0;
    for i in 0..300 {
        let n = g.node().unwrap();
        g.link(n, kind, person).unwrap();
        let v = g.value(format!("person{i}").as_bytes()).unwrap();
        g.link(n, name, v).unwrap();
        if i == 123 {
            bob = n;
        }
    }
    let bob_name = g.find_value(b"person123").unwrap();
    let mut q = Query::new();
    let x = q.var("x");
    q.pattern(x, kind, person).pattern(x, name, bob_name);
    let plan = q.plan(&g).unwrap();
    let (pattern, access) = first_step(&plan);
    assert_eq!(pattern.object, Term::Id(bob_name), "{}", plan.explain());
    assert_eq!(access, Access::PredicateObject);
    assert!(plan.estimated_rows() < 10.0, "{}", plan.explain());
    assert!(
        plan.explain().starts_with("scan (?x "),
        "{}",
        plan.explain()
    );
    assert_eq!(q.run(&g).unwrap(), [[bob]]);
}

#[test]
fn large_results_and_limit() {
    let mut g = graph();
    let (p, q_) = (g.value(b"p").unwrap(), g.value(b"q").unwrap());
    let hub = g.node().unwrap();
    for _ in 0..3000 {
        let n = g.node().unwrap();
        g.link(hub, p, n).unwrap();
        g.link(n, q_, hub).unwrap();
    }
    let mut q = Query::new();
    let (x, y) = (q.var("x"), q.var("y"));
    q.pattern(hub, p, x).pattern(x, q_, y);
    for force in [None, Some(Join::Bind), Some(Join::Hash)] {
        let plan = plan::plan_with(&q, &g, force).unwrap();
        let rows: Vec<Vec<u64>> = plan.execute(&g).collect::<Result<_>>().unwrap();
        assert_eq!(rows.len(), 3000);
        assert!(rows.iter().all(|r| r[1] == hub));
    }

    // Distinct and ordered.
    let mut q2 = q.clone();
    q2.select([y]).distinct();
    assert_eq!(q2.run(&g).unwrap(), [[hub]]);
    let mut q2 = q.clone();
    q2.order_by(SortKey::id(x).desc()).limit(3);
    let rows = q2.run(&g).unwrap();
    assert_eq!(rows.len(), 3);
    assert!(rows[0][0] > rows[1][0] && rows[1][0] > rows[2][0]);

    q.limit(5);
    assert_eq!(q.run(&g).unwrap().len(), 5);
    q.limit(0);
    assert!(q.run(&g).unwrap().is_empty());
    // Taking a few rows stops early.
    let mut q = Query::new();
    let x = q.var("x");
    q.pattern(hub, p, x);
    assert_eq!(q.plan(&g).unwrap().execute(&g).take(3).count(), 3);
}

#[test]
fn constants_links_and_filters() {
    let mut g = graph();
    let (a, b) = (g.node().unwrap(), g.node().unwrap());
    let (p, said, age) = (
        g.value(b"p").unwrap(),
        g.value(b"said").unwrap(),
        g.value(b"age").unwrap(),
    );
    let l = g.link(a, p, b).unwrap();
    let carol = g.node().unwrap();
    g.link(carol, said, l).unwrap();
    let v40 = g.value(b"40").unwrap();
    let v7 = g.value(b"7").unwrap();
    g.link(a, age, v40).unwrap();
    g.link(b, age, v7).unwrap();

    // A query with no variables: does the link hold?
    let mut q = Query::new();
    q.pattern(a, p, b);
    assert_eq!(q.run(&g).unwrap(), [Vec::<u64>::new()]);
    let mut q = Query::new();
    q.pattern(b, p, a);
    assert!(q.run(&g).unwrap().is_empty());

    // Who said that a p b?
    let mut q = Query::new();
    let (link, who) = (q.var("link"), q.var("who"));
    q.link_pattern(link, a, p, b)
        .pattern(who, said, link)
        .select([who]);
    assert_eq!(q.run(&g).unwrap(), [[carol]]);
    check(&q, &g);

    // After deleting it, nobody: the link pattern matches live links only.
    g.delete_link(a, p, b).unwrap();
    assert!(q.run(&g).unwrap().is_empty());
    check(&q, &g);

    // Tests of values.
    let mut q = Query::new();
    let (x, v) = (q.var("x"), q.var("v"));
    let adult: Test = Arc::new(|g: &Graph, id| {
        g.data(id)
            .and_then(|d| std::str::from_utf8(d).ok()?.parse::<u32>().ok())
            .is_some_and(|n| n >= 18)
    });
    q.pattern(x, age, v)
        .filter(Filter::Test(v, adult))
        .select([x]);
    assert_eq!(q.run(&g).unwrap(), [[a]]);
    let mut q2 = Query::new();
    let (x, v) = (q2.var("x"), q2.var("v"));
    q2.pattern(x, age, v)
        .filter(Filter::Eq(v.into(), v7.into()));
    assert_eq!(q2.run(&g).unwrap(), [[b, v7]]);
    // A filter without variables.
    q2.filter(Filter::Eq(Term::Id(1), Term::Id(2)));
    assert!(q2.run(&g).unwrap().is_empty());
    assert!(q2.plan(&g).unwrap().explain().starts_with("filter 1 = 2"));
}

#[test]
fn repeated_variables_and_predicate_variables() {
    let mut g = graph();
    let (a, b) = (g.node().unwrap(), g.node().unwrap());
    let (p, r) = (g.value(b"p").unwrap(), g.value(b"r").unwrap());
    g.link(a, p, a).unwrap();
    g.link(a, p, b).unwrap();
    g.link(a, r, b).unwrap();

    let mut q = Query::new();
    let x = q.var("x");
    q.pattern(x, p, x);
    assert_eq!(q.run(&g).unwrap(), [[a]]);
    check(&q, &g);

    // Which predicates link a to b?
    let mut q = Query::new();
    let pred = q.var("pred");
    q.pattern(a, pred, b);
    assert_eq!(sorted(q.run(&g).unwrap()), [[p], [r]]);
    // A scan of every link.
    let mut q = Query::new();
    let (s, pred, o) = (q.var("s"), q.var("pred"), q.var("o"));
    q.pattern(s, pred, o);
    assert_eq!(first_step(&q.plan(&g).unwrap()).1, Access::All);
    assert_eq!(q.run(&g).unwrap().len(), 3);
    check(&q, &g);
}

/// People with names, some with nicknames, some banned, some with ages.
fn people() -> (Graph, Vec<u64>, [u64; 5]) {
    let mut g = graph();
    let [name, nick, banned, age, yes] =
        [&b"name"[..], b"nick", b"banned", b"age", b"yes"].map(|d| g.value(d).unwrap());
    let mut people = Vec::new();
    for (i, n) in ["carol", "alice", "dave", "bob"].iter().enumerate() {
        let p = g.node().unwrap();
        let v = g.value(n.as_bytes()).unwrap();
        g.link(p, name, v).unwrap();
        if i % 2 == 0 {
            let v = g.value(format!("{n}-nick").as_bytes()).unwrap();
            g.link(p, nick, v).unwrap();
        }
        if i == 2 {
            g.link(p, banned, yes).unwrap();
        }
        if i < 3 {
            let v = g.value(format!("{}", 20 + i).as_bytes()).unwrap();
            g.link(p, age, v).unwrap();
        }
        people.push(p);
    }
    (g, people, [name, nick, banned, age, yes])
}

#[test]
fn unions() {
    let (g, people, [name, nick, ..]) = people();
    // Every name or nickname.
    let mut q = Query::new();
    let (x, n) = (q.var("x"), q.var("n"));
    let mut by_name = Group::new();
    by_name.pattern(x, name, n);
    let mut by_nick = Group::new();
    by_nick.pattern(x, nick, n);
    q.union([by_name, by_nick]).order_by(SortKey::data(n));
    let rows = q.run(&g).unwrap();
    let data: Vec<&[u8]> = rows.iter().map(|r| g.data(r[1]).unwrap()).collect();
    assert_eq!(
        data,
        [
            &b"alice"[..],
            b"bob",
            b"carol",
            b"carol-nick",
            b"dave",
            b"dave-nick"
        ]
    );
    check(&q, &g);

    // A union joined to a pattern; each person once.
    let mut q2 = q.clone();
    let full_name = q2.var("name");
    q2.pattern(x, name, full_name);
    check(&q2, &g);
    q2.select([x]).distinct();
    assert_eq!(
        sorted(q2.run(&g).unwrap()),
        people.iter().map(|&p| vec![p]).collect::<Vec<_>>()
    );

    // Branches must bind the same variables.
    let mut q = Query::new();
    let (x, n) = (q.var("x"), q.var("n"));
    let mut a = Group::new();
    a.pattern(x, name, n);
    let mut b = Group::new();
    b.pattern(x, nick, 1);
    q.union([a, b]);
    assert!(matches!(q.plan(&g), Err(Error::InvalidArgument(_))));
    let mut q = Query::new();
    q.union([]);
    assert!(matches!(q.plan(&g), Err(Error::InvalidArgument(_))));
}

#[test]
fn negation() {
    let (g, people, [name, nick, banned, age, yes]) = people();
    // People who are not banned.
    let mut q = Query::new();
    let (x, n) = (q.var("x"), q.var("n"));
    let mut is_banned = Group::new();
    is_banned.pattern(x, banned, yes);
    q.pattern(x, name, n).not(is_banned).select([x]);
    let expect: Vec<Vec<u64>> = [0, 1, 3].iter().map(|&i| vec![people[i]]).collect();
    assert_eq!(sorted(q.run(&g).unwrap()), expect);
    check(&q, &g);
    for force in [Join::Bind, Join::Hash] {
        let plan = plan::plan_with(&q, &g, Some(force)).unwrap();
        let how = if force == Join::Bind {
            "for the rows"
        } else {
            "once"
        };
        assert!(plan.explain().contains(how), "{}", plan.explain());
    }

    // People without a nickname, using the negation's own variable.
    let mut q = Query::new();
    let (x, n, any) = (q.var("x"), q.var("n"), q.var("any"));
    let mut has_nick = Group::new();
    has_nick.pattern(x, nick, any);
    q.pattern(x, name, n).not(has_nick).select([x]);
    let expect: Vec<Vec<u64>> = [1, 3].iter().map(|&i| vec![people[i]]).collect();
    assert_eq!(sorted(q.run(&g).unwrap()), expect);
    check(&q, &g);

    // A negation with a filter using an outer variable: people with no
    // younger person.  It can only be evaluated for the rows.
    let mut q = Query::new();
    let (x, a, y, b) = (q.var("x"), q.var("a"), q.var("y"), q.var("b"));
    let mut younger = Group::new();
    younger
        .pattern(y, age, b)
        .filter(Filter::Lt(b.into(), a.into()));
    q.pattern(x, age, a).not(younger).select([x]);
    // Values were appended in age order, so the youngest has the least id.
    assert_eq!(q.run(&g).unwrap(), [[people[0]]]);
    check(&q, &g);
    let plan = plan::plan_with(&q, &g, Some(Join::Hash)).unwrap();
    assert!(
        plan.explain().contains("for the rows"),
        "{}",
        plan.explain()
    );

    // A negation sharing no variables removes all rows or none.
    let mut q = Query::new();
    let x = q.var("x");
    let mut anyone_banned = Group::new();
    anyone_banned.pattern(Term::Var(q.var("y")), banned, yes);
    let n = q.var("n");
    q.pattern(x, name, n).not(anyone_banned);
    assert!(q.run(&g).unwrap().is_empty());
    check(&q, &g);
}

#[test]
fn pattern_as_of() {
    let mut g = graph();
    let (a, b, c) = (g.node().unwrap(), g.node().unwrap(), g.node().unwrap());
    let knows = g.value(b"knows").unwrap();
    g.link(a, knows, b).unwrap();
    let then = g.next_id() - 1;
    g.link(a, knows, c).unwrap();
    g.delete_link(a, knows, b).unwrap();

    // Who does a know now that it did not then?
    let mut q = Query::new();
    let x = q.var("x");
    let mut before = Group::new();
    before.pattern(a, knows, x).at(then);
    q.pattern(a, knows, x).not(before);
    assert_eq!(q.run(&g).unwrap(), [[c]]);
    check(&q, &g);
    // And who did a know then that it does not now?
    let mut q = Query::new();
    let x = q.var("x");
    let mut now = Group::new();
    now.pattern(a, knows, x);
    q.pattern(a, knows, x).at(then).not(now);
    assert_eq!(q.run(&g).unwrap(), [[b]]);
    assert!(
        q.plan(&g)
            .unwrap()
            .explain()
            .contains(&format!("as of {then}"))
    );
    check(&q, &g);
}

#[test]
fn many_patterns_are_planned_greedily() {
    let mut g = graph();
    let next = g.value(b"next").unwrap();
    let n: Vec<u64> = (0..20).map(|_| g.node().unwrap()).collect();
    for w in n.windows(2) {
        g.link(w[0], next, w[1]).unwrap();
    }
    let mut q = Query::new();
    let vars: Vec<Var> = (0..=EXHAUSTIVE_PATTERNS + 2)
        .map(|i| q.var(&format!("v{i}")))
        .collect();
    q.pattern(n[0], next, vars[0]);
    for w in vars.windows(2) {
        q.pattern(w[0], next, w[1]);
    }
    assert!(q.patterns().len() > EXHAUSTIVE_PATTERNS);
    let rows = q.run(&g).unwrap();
    assert_eq!(rows, [n[1..=vars.len()].to_vec()]);
}

#[test]
fn errors() {
    let g = graph();
    assert!(matches!(
        Query::new().plan(&g),
        Err(Error::InvalidArgument(_))
    ));
    let mut q = Query::new();
    let (x, y) = (q.var("x"), q.var("y"));
    q.pattern(x, 1, 2).filter(Filter::Ne(x.into(), y.into()));
    assert!(matches!(q.plan(&g), Err(Error::InvalidArgument(_))));
    let mut q = Query::new();
    let (x, y) = (q.var("x"), q.var("y"));
    q.pattern(x, 1, 2).select([y]);
    assert!(matches!(q.plan(&g), Err(Error::InvalidArgument(_))));
    let mut q = Query::new();
    let (x, y) = (q.var("x"), q.var("y"));
    q.pattern(x, 1, 2).order_by(SortKey::id(y));
    assert!(matches!(q.plan(&g), Err(Error::InvalidArgument(_))));
    // A negation must have a pattern.
    let mut q = Query::new();
    let x = q.var("x");
    q.pattern(x, 1, 2).not(Group::new());
    assert!(matches!(q.plan(&g), Err(Error::InvalidArgument(_))));
}
