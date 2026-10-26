use std::sync::Arc;

use super::plan::{Access, Join};
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

/// The rows of `q`, ignoring its limit, by trying every combination of links.
fn brute_force(q: &Query, g: &Graph) -> Vec<Vec<u64>> {
    fn search(
        q: &Query,
        g: &Graph,
        links: &[(u64, [u64; 3])],
        i: usize,
        row: &mut Vec<Option<u64>>,
        out: &mut Vec<Vec<u64>>,
    ) {
        if i == q.patterns.len() {
            let full: Vec<u64> = row.iter().map(|v| v.unwrap_or(0)).collect();
            if q.filters.iter().all(|f| f.eval(g, &full)) {
                out.push(q.selected().iter().map(|v| full[v.0]).collect());
            }
            return;
        }
        let p = q.patterns[i];
        for &(id, spo) in links {
            let saved = row.clone();
            let fields = [
                (Some(p.subject), spo[0]),
                (Some(p.predicate), spo[1]),
                (Some(p.object), spo[2]),
                (p.link.map(Term::Var), id),
            ];
            let ok = fields.iter().all(|&(term, value)| match term {
                None => true,
                Some(Term::Id(c)) => c == value,
                Some(Term::Var(v)) => match row[v.0] {
                    Some(bound) => bound == value,
                    None => {
                        row[v.0] = Some(value);
                        true
                    }
                },
            });
            if ok {
                search(q, g, links, i + 1, row, out);
            }
            *row = saved;
        }
    }
    let links = live_links(g, q.as_of);
    let mut out = Vec::new();
    search(q, g, &links, 0, &mut vec![None; q.names.len()], &mut out);
    out.sort();
    out
}

fn sorted(mut rows: Vec<Vec<u64>>) -> Vec<Vec<u64>> {
    rows.sort();
    rows
}

/// `plan` with every step after the first joined by `join` where it can be.
fn force(plan: &Plan, join: Join) -> Plan {
    let mut plan = plan.clone();
    let q = &plan.query;
    let mut bound = vec![false; q.names.len()];
    for (i, step) in plan.steps.iter_mut().enumerate() {
        let p = q.patterns[step.pattern];
        let shared = !plan::shared_vars(&p, &bound).is_empty();
        step.join = if i > 0 && shared { join } else { Join::Bind };
        let is_bound = |t: Term| match t {
            Term::Id(_) => true,
            Term::Var(v) => step.join == Join::Bind && bound[v.0],
        };
        let link = step.join == Join::Bind && p.link.is_some_and(|v| bound[v.0]);
        step.access = Access::of(link, p.terms().map(is_bound));
        p.vars().for_each(|v| bound[v.0] = true);
    }
    plan
}

/// Checks `q` against brute force, with the plan chosen and with each join
/// forced.
fn check(q: &Query, g: &Graph) {
    let expected = brute_force(q, g);
    let plan = q.plan(g).unwrap();
    for plan in [
        plan.clone(),
        force(&plan, Join::Bind),
        force(&plan, Join::Hash),
    ] {
        let rows: Vec<Vec<u64>> = plan.execute(g).collect::<Result<_>>().unwrap();
        assert_eq!(sorted(rows), expected, "{q:?}\n{}", plan.explain());
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

/// A random query of up to four patterns over up to four variables.
fn random_query(rng: &mut Rng, nodes: &[u64], predicates: &[u64]) -> Query {
    let mut q = Query::new();
    let mut vars: Vec<Var> = Vec::new();
    let mut term = |q: &mut Query, rng: &mut Rng, consts: &[u64], var_percent| {
        if rng.chance(var_percent) {
            if vars.is_empty() || (vars.len() < 4 && rng.chance(40)) {
                vars.push(q.var(&format!("v{}", vars.len())));
            }
            Term::Var(rng.pick(&vars))
        } else {
            Term::Id(rng.pick(consts))
        }
    };
    for _ in 0..1 + rng.below(4) {
        let s = term(&mut q, rng, nodes, 70);
        let p = term(&mut q, rng, predicates, 20);
        let o = term(&mut q, rng, nodes, 70);
        q.pattern(s, p, o);
        if rng.chance(15) {
            let link = q.var("link");
            q.patterns.last_mut().unwrap().link = Some(link);
        }
    }
    // Variables made but not used in a pattern are dropped by binding them.
    let used: Vec<Var> = q.patterns.iter().flat_map(Pattern::vars).collect();
    let mut q2 = q.clone();
    q2.select = Some(used.clone());
    if used.len() >= 2 && rng.chance(40) {
        let (a, b) = (rng.pick(&used), rng.pick(&used));
        q2.filter(if rng.chance(50) {
            Filter::Ne(a.into(), b.into())
        } else {
            Filter::Lt(a.into(), b.into())
        });
    }
    q2
}

#[test]
fn random_queries() {
    let mut rng = Rng(0x9e37_79b9_7f4a_7c15);
    for _ in 0..8 {
        let (g, nodes, predicates) = random_graph(&mut rng);
        for _ in 0..60 {
            let mut q = random_query(&mut rng, &nodes, &predicates);
            check(&q, &g);
            q.as_of(g.start_id() + rng.next() % (g.next_id() - g.start_id()));
            check(&q, &g);
        }
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

    // Triangles: n0 -> n2 -> n0 is not one; n0 -> n1 -> n2 -> n0 is.
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
    assert_eq!(plan.steps[0].pattern, 1, "{}", plan.explain());
    assert_eq!(plan.steps[0].access, Access::PredicateObject);
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
    let mut spokes = Vec::new();
    for _ in 0..3000 {
        let n = g.node().unwrap();
        g.link(hub, p, n).unwrap();
        g.link(n, q_, hub).unwrap();
        spokes.push(n);
    }
    let mut q = Query::new();
    let (x, y) = (q.var("x"), q.var("y"));
    q.pattern(hub, p, x).pattern(x, q_, y);
    let plan = q.plan(&g).unwrap();
    for plan in [
        plan.clone(),
        force(&plan, Join::Bind),
        force(&plan, Join::Hash),
    ] {
        let rows: Vec<Vec<u64>> = plan.execute(&g).collect::<Result<_>>().unwrap();
        assert_eq!(rows.len(), 3000);
        assert!(rows.iter().all(|r| r[1] == hub));
    }

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

    // Which predicates link a to b?  A scan of every link.
    let mut q = Query::new();
    let pred = q.var("pred");
    q.pattern(a, pred, b);
    assert_eq!(sorted(q.run(&g).unwrap()), [[p], [r]]);
    let mut q = Query::new();
    let (s, pred, o) = (q.var("s"), q.var("pred"), q.var("o"));
    q.pattern(s, pred, o);
    assert_eq!(q.plan(&g).unwrap().steps[0].access, Access::All);
    assert_eq!(q.run(&g).unwrap().len(), 3);
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
}
