use std::fs;
use std::path::PathBuf;

use super::*;

/// A temporary directory removed on drop.
struct TempDir(PathBuf);

impl TempDir {
    fn new(name: &str) -> Self {
        let dir = std::env::temp_dir().join(format!("gdblite-graph-{}-{name}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        TempDir(dir)
    }

    fn prefix(&self, name: &str) -> PathBuf {
        self.0.join(format!("{name}."))
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn set(ids: &[u64]) -> IdSet {
    ids.iter().copied().collect()
}

fn graph() -> Graph {
    Graph::open(None, Options::default()).unwrap()
}

#[test]
fn value_interning() {
    let mut g = graph();
    let a = g.value(b"alice").unwrap();
    let b = g.value(b"bob").unwrap();
    assert_ne!(a, b);
    assert_eq!(g.value(b"alice").unwrap(), a);
    assert_eq!(g.find_value(b"bob"), Some(b));
    assert_eq!(g.find_value(b"carol"), None);
    assert_eq!(g.data(a), Some(&b"alice"[..]));
    // Empty data is a node, and nodes are not interned.
    let n = g.value(b"").unwrap();
    assert!(g.get(n).unwrap().is_node());
    assert_ne!(g.node().unwrap(), n);
    assert_eq!(g.data(n), None);
}

/// foo -> bar -> baz, by `prop`.
fn chain(g: &mut Graph) -> (u64, u64, u64, u64) {
    let prop = g.value(b"gloss").unwrap();
    let (foo, bar, baz) = (g.node().unwrap(), g.node().unwrap(), g.node().unwrap());
    g.link(foo, prop, bar).unwrap();
    g.link(bar, prop, baz).unwrap();
    (prop, foo, bar, baz)
}

#[test]
fn follow_links() {
    let mut g = graph();
    let (prop, foo, bar, baz) = chain(&mut g);
    assert_eq!(g.follow_forward(prop, [bar]), set(&[baz]));
    assert_eq!(g.follow_backward(prop, [bar]), set(&[foo]));
    assert_eq!(g.follow_forward(prop, [foo, bar]), set(&[bar, baz]));
    assert!(g.follow_forward(prop, [baz]).is_empty());
    let other = g.value(b"other").unwrap();
    assert!(g.follow_forward(other, [foo]).is_empty());
}

#[test]
fn link_dedup() {
    let mut g = graph();
    let (prop, foo, bar, _) = chain(&mut g);
    let l = g.find_link(foo, prop, bar).unwrap();
    // Re-asserting a live link returns it, whatever its other fields.
    assert_eq!(g.link(foo, prop, bar).unwrap(), l);
    let author = g.node().unwrap();
    let again = LinkFields {
        subject: foo,
        predicate: prop,
        object: bar,
        author,
        ..LinkFields::default()
    };
    assert_eq!(g.add_link(&again).unwrap(), l);
    assert_eq!(g.find_links(foo, prop, bar), set(&[l]));
}

#[test]
fn delete_and_recreate() {
    let mut g = graph();
    let (prop, foo, bar, _) = chain(&mut g);
    let l1 = g.find_link(foo, prop, bar).unwrap();

    let d1 = g.delete_link(foo, prop, bar).unwrap().unwrap();
    assert!(g.get(d1).unwrap().as_link().unwrap().dead());
    assert_eq!(g.find_link(foo, prop, bar), None);
    // Deleting again does nothing.
    assert_eq!(g.delete_link(foo, prop, bar).unwrap(), None);
    assert_eq!(g.find_links(foo, prop, bar), set(&[l1, d1]));
    assert!(g.follow_forward(prop, [foo]).is_empty());

    let l2 = g.link(foo, prop, bar).unwrap();
    assert!(l2 > d1);
    assert_eq!(g.find_link(foo, prop, bar), Some(l2));
    assert_eq!(g.follow_forward(prop, [foo]), set(&[bar]));

    let d2 = g.delete_link(foo, prop, bar).unwrap().unwrap();
    assert_eq!(g.find_link(foo, prop, bar), None);
    assert_eq!(g.find_links(foo, prop, bar), set(&[l1, d1, l2, d2]));
}

#[test]
fn write_dead() {
    // A link can be written dead to begin with.
    let mut g = graph();
    let p1 = g.value(b"p1").unwrap();
    let (foo, bar) = (g.node().unwrap(), g.node().unwrap());
    let dead = LinkFields {
        subject: foo,
        predicate: p1,
        object: bar,
        state: LinkState::Dead,
        ..LinkFields::default()
    };
    let l = g.add_link(&dead).unwrap();
    assert_eq!(g.add_link(&dead).unwrap(), l);
    assert!(g.get(l).unwrap().as_link().unwrap().dead());
    assert_eq!(g.find_link(foo, p1, bar), None);
    let all = g.links_at(Linkage::Predicate, [p1]);
    assert_eq!(all, set(&[l]));
    assert!(g.live(&all, None).is_empty());
}

#[test]
fn live_and_as_of() {
    // Several links with a predicate, all but one deleted.
    let mut g = graph();
    let p1 = g.value(b"p1").unwrap();
    let nodes: Vec<u64> = (0..3).map(|_| g.node().unwrap()).collect();
    let l0 = g.link(nodes[0], p1, nodes[1]).unwrap();
    let l1 = g.link(nodes[1], p1, nodes[2]).unwrap();
    let before = g.next_id() - 1;
    let d0 = g.delete_link(nodes[0], p1, nodes[1]).unwrap().unwrap();

    let all = g.links_at(Linkage::Predicate, [p1]);
    assert_eq!(all, set(&[l0, l1, d0]));
    assert_eq!(g.live(&all, None), set(&[l1]));
    // Before the delete, both were live.
    assert_eq!(g.live(&all, Some(before)), set(&[l0, l1]));
    assert_eq!(g.live(&all, Some(l0)), set(&[l0]));
    assert!(g.live(&all, Some(p1)).is_empty());

    assert_eq!(
        g.query_property([nodes[0]], Linkage::Subject, p1, Some(before))
            .unwrap(),
        (set(&[l0]), set(&[nodes[1]]))
    );
    assert_eq!(
        g.query_property([nodes[0]], Linkage::Subject, p1, None)
            .unwrap(),
        (IdSet::new(), IdSet::new())
    );
    assert!(matches!(
        g.query_property([nodes[0]], Linkage::Author, p1, None),
        Err(Error::InvalidArgument(_))
    ));
}

#[test]
fn entities_and_projection() {
    let mut g = graph();
    let name = g.value(b"name").unwrap();
    let knows = g.value(b"knows").unwrap();
    let (alice, bob, carol) = (g.node().unwrap(), g.node().unwrap(), g.node().unwrap());
    let alice_name = g.value(b"Alice").unwrap();
    let author = g.node().unwrap();
    let (sg1, sg2) = (g.node().unwrap(), g.node().unwrap());
    g.link(alice, name, alice_name).unwrap();
    let k1 = g
        .add_link(&LinkFields {
            subject: alice,
            predicate: knows,
            object: bob,
            author,
            when: 1000,
            subgraphs: vec![sg1, sg2],
            ..LinkFields::default()
        })
        .unwrap();
    let k2 = g.link(alice, knows, carol).unwrap();

    assert_eq!(
        g.entities_with(Linkage::Subject, alice, Linkage::Object, None),
        set(&[alice_name, bob, carol])
    );
    assert_eq!(
        g.entities_with(Linkage::Object, bob, Linkage::Subject, None),
        set(&[alice])
    );
    assert_eq!(
        g.entity_with(Linkage::Subject, alice, Linkage::Object, None),
        Some(carol)
    );
    assert_eq!(
        g.entity_with(Linkage::Subject, alice, Linkage::Object, Some(k1)),
        Some(bob)
    );
    assert_eq!(
        g.entity_with(Linkage::Subject, bob, Linkage::Object, None),
        None
    );

    let knows_links = g.links_at(Linkage::Predicate, [knows]);
    assert_eq!(knows_links, set(&[k1, k2]));
    assert_eq!(g.project(&knows_links, Linkage::Object), set(&[bob, carol]));
    assert_eq!(g.project(&knows_links, Linkage::Author), set(&[author]));
    assert_eq!(g.project(&knows_links, Linkage::Subgraph), set(&[sg1, sg2]));
    assert_eq!(
        g.project_map(&knows_links, Linkage::Object),
        BTreeMap::from([(k1, bob), (k2, carol)])
    );
    assert_eq!(g.links_at(Linkage::Subgraph, [sg2]), set(&[k1]));
    assert_eq!(g.links_at(Linkage::Author, [author]), set(&[k1]));
    assert!(g.links_at(Linkage::When, [1000]).is_empty());
}

#[test]
fn forward_reference() {
    let mut g = graph();
    let a = g.node().unwrap();
    let err = g.link(a, a, g.next_id());
    assert!(matches!(
        err,
        Err(Error::Record(crate::Error::ForwardReference))
    ));
}

#[test]
fn batch() {
    let dir = TempDir::new("batch");
    let prefix = dir.prefix("g");
    let mut g = Graph::open(Some(&prefix), Options::default()).unwrap();
    let (prop, foo, bar, _) = g.batch(|g| Ok(chain(g))).unwrap();

    // A failed batch leaves nothing behind.
    let lost = g.next_id();
    let err = g.batch(|g| {
        let v = g.value(b"lost")?;
        g.link(foo, prop, v)?;
        g.delete_link(foo, prop, bar)?;
        Err::<(), _>(Error::InvalidArgument("give up"))
    });
    assert!(matches!(err, Err(Error::InvalidArgument("give up"))));
    assert_eq!(g.find_value(b"lost"), None);
    assert_eq!(g.follow_forward(prop, [foo]), set(&[bar]));
    assert!(g.links_at(Linkage::Subject, [foo]).len() == 1);
    // Its ids are not reused.
    let v = g.value(b"lost").unwrap();
    assert!(v > lost);
    g.commit().unwrap();
    drop(g);

    let g = Graph::open(Some(&prefix), Options::default()).unwrap();
    assert_eq!(g.find_value(b"lost"), Some(v));
    assert_eq!(g.follow_forward(prop, [foo]), set(&[bar]));
}

#[test]
fn uncommitted_entries_are_lost() {
    let dir = TempDir::new("uncommitted");
    let prefix = dir.prefix("g");
    let (prop, foo, bar);
    {
        let mut g = Graph::open(Some(&prefix), Options::default()).unwrap();
        (prop, foo, bar, _) = chain(&mut g);
        g.commit().unwrap();
        let v = g.value(b"uncommitted").unwrap();
        g.link(foo, prop, v).unwrap();
        g.delete_link(foo, prop, bar).unwrap();
    }
    // The indexes still name the lost entries, but the graph ignores them.
    let mut g = Graph::open(Some(&prefix), Options::default()).unwrap();
    assert_eq!(g.find_value(b"uncommitted"), None);
    assert_eq!(g.links_at(Linkage::Subject, [foo]).len(), 1);
    assert_eq!(g.follow_forward(prop, [foo]), set(&[bar]));
    assert!(g.find_link(foo, prop, bar).is_some());
    let v = g.value(b"uncommitted").unwrap();
    assert_eq!(g.find_value(b"uncommitted"), Some(v));
    g.link(foo, prop, v).unwrap();
    assert_eq!(g.follow_forward(prop, [foo]), set(&[bar, v]));
}

#[test]
fn snapshot_restore() {
    let mut g = graph();
    let (prop, foo, bar, baz) = chain(&mut g);
    g.delete_link(bar, prop, baz).unwrap();
    let mut bytes = Vec::new();
    g.snapshot(&mut bytes).unwrap();

    let dir = TempDir::new("snapshot");
    let prefix = dir.prefix("g");
    let restored = Graph::restore(Some(&prefix), Options::default(), &mut &bytes[..]).unwrap();
    assert!(!restored.is_transient());
    assert_eq!(restored.next_id(), g.next_id());
    assert_eq!(restored.find_value(b"gloss"), Some(prop));
    assert_eq!(restored.follow_forward(prop, [foo]), set(&[bar]));
    assert!(restored.follow_forward(prop, [bar]).is_empty());
    assert_eq!(
        restored.records(restored.start_id()).count(),
        g.records(g.start_id()).count()
    );
}
