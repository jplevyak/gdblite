use std::fs;
use std::path::PathBuf;

use super::*;
use crate::local::{self, Options};
use crate::record::LinkFields;

/// A temporary directory removed on drop.
struct TempDir(PathBuf);

impl TempDir {
    fn new(name: &str) -> Self {
        let dir = std::env::temp_dir().join(format!("gdblite-ti-{}-{name}", std::process::id()));
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

/// Appends a link, returning its id.
fn link(local: &mut Local, fields: LinkFields) -> u64 {
    let r = Record::link(local.log().next_id(), &fields).unwrap();
    local.log_mut().append(&r).unwrap();
    r.id()
}

#[test]
fn duplicate_value() {
    let mut local = Local::open(None, Options::default()).unwrap();
    let log = local.log_mut();
    let (a, b, c) = (
        log.push_value(b"foo").unwrap(),
        log.push_value(b"foo").unwrap(),
        log.push_value(b"foo").unwrap(),
    );
    let records: Vec<_> = [a, b, c]
        .iter()
        .map(|&id| local.log().get(id).unwrap().into_owned())
        .collect();
    let err = index_records(&mut local, &records);
    assert!(matches!(
        err,
        Err(Error::DuplicateValue { id, existing }) if (id, existing) == (b, a)
    ));
    // Nothing was indexed.
    assert_eq!(find_value(&local, b"foo"), None);

    index_records(&mut local, &records[..1]).unwrap();
    assert_eq!(find_value(&local, b"foo"), Some(a));
    // Indexing the same value again is fine; a new one with its data is not.
    index_records(&mut local, &records[..1]).unwrap();
    let err = index_records(&mut local, &records[2..]);
    assert!(matches!(
        err,
        Err(Error::DuplicateValue { id, existing }) if (id, existing) == (c, a)
    ));

    // An entry that no longer names a value, as after a crash, is replaced.
    let node = local.log_mut().push_node().unwrap();
    local
        .table_mut(BuiltinIndex::Value.table())
        .set(Uuid::of(b"bar"), node)
        .unwrap();
    let bar = local.log_mut().push_value(b"bar").unwrap();
    let record = local.log().get(bar).unwrap().into_owned();
    index_records(&mut local, &[record]).unwrap();
    assert_eq!(find_value(&local, b"bar"), Some(bar));
}

/// The ids of a small graph.
struct Graph {
    alice: u64,
    bob: u64,
    carol: u64,
    knows: u64,
    likes: u64,
    sg1: u64,
    sg2: u64,
    links: [u64; 4],
}

fn build(local: &mut Local) -> Graph {
    let mut value = |data: &str| local.log_mut().push_value(data.as_bytes()).unwrap();
    let (alice, bob, carol) = (value("alice"), value("bob"), value("carol"));
    let (knows, likes) = (value("knows"), value("likes"));
    let (sg1, sg2) = (value("sg1"), value("sg2"));
    let f = |subject, predicate, object| LinkFields {
        subject,
        predicate,
        object,
        when: 1_700_000_000_000,
        ..LinkFields::default()
    };
    let links = [
        link(
            local,
            LinkFields {
                author: carol,
                subgraphs: vec![sg1, sg2],
                ..f(alice, knows, bob)
            },
        ),
        link(local, f(alice, likes, bob)),
        link(local, f(alice, knows, carol)),
        link(local, f(alice, knows, bob)),
    ];
    Graph {
        alice,
        bob,
        carol,
        knows,
        likes,
        sg1,
        sg2,
        links,
    }
}

fn check(local: &Local, g: &Graph) {
    let [l1, l2, l3, l4] = g.links;
    assert_eq!(find_value(local, b"alice"), Some(g.alice));
    assert_eq!(find_value(local, b"likes"), Some(g.likes));
    assert_eq!(find_value_by_uuid(local, Uuid::of(b"bob")), Some(g.bob));
    assert_eq!(find_value(local, b"zed"), None);

    assert_eq!(
        find_links(local, Linkage::Subject, g.alice),
        set(&[l1, l2, l3, l4])
    );
    assert_eq!(
        find_links(local, Linkage::Predicate, g.knows),
        set(&[l1, l3, l4])
    );
    assert_eq!(
        find_links(local, Linkage::Object, g.bob),
        set(&[l1, l2, l4])
    );
    assert_eq!(find_links(local, Linkage::Author, g.carol), set(&[l1]));
    // Every subgraph is indexed, not just the first.
    assert_eq!(find_links(local, Linkage::Subgraph, g.sg1), set(&[l1]));
    assert_eq!(find_links(local, Linkage::Subgraph, g.sg2), set(&[l1]));
    assert!(find_links(local, Linkage::Subject, g.bob).is_empty());

    assert_eq!(
        find_links_by_spo(local, g.alice, g.knows, g.bob),
        set(&[l1, l4])
    );
    assert_eq!(find_link_by_spo(local, g.alice, g.knows, g.bob), Some(l4));
    assert_eq!(find_link_by_spo(local, g.bob, g.knows, g.alice), None);

    let sp = Intersection::of(Linkage::Subject, Linkage::Predicate).unwrap();
    let po = Intersection::of(Linkage::Predicate, Linkage::Object).unwrap();
    assert_eq!(
        find_links_by_intersection(local, sp, g.alice, g.knows),
        set(&[l1, l3, l4])
    );
    assert_eq!(
        find_links_by_intersection(local, po, g.knows, g.bob),
        set(&[l1, l4])
    );
    assert_eq!(
        find_link_by_intersection(local, po, g.likes, g.bob),
        Some(l2)
    );
    assert_eq!(find_link_by_intersection(local, po, g.likes, g.carol), None);
}

#[test]
fn graph() {
    let mut local = Local::open(None, Options::default()).unwrap();
    let start = local.log().start_id();
    let g = build(&mut local);
    index_log(&mut local, start).unwrap();
    check(&local, &g);
}

#[test]
fn records_from_elsewhere() {
    let mut local = Local::open(None, Options::default()).unwrap();
    let g = build(&mut local);
    let records: Vec<Record<'static>> = local
        .log()
        .records(local.log().start_id())
        .map(|r| r.unwrap().into_owned())
        .collect();
    index_records(&mut local, &records).unwrap();
    check(&local, &g);
}

#[test]
fn persistent() {
    let dir = TempDir::new("persistent");
    let prefix = dir.prefix("db");
    let g = {
        let mut local = Local::open(Some(&prefix), Options::default()).unwrap();
        let start = local.log().start_id();
        let g = build(&mut local);
        index_log(&mut local, start).unwrap();
        local.commit().unwrap();
        g
    };
    let local = Local::open(Some(&prefix), Options::default()).unwrap();
    assert_eq!(local.table_count(), NUM_BUILTIN_INDEXES);
    check(&local, &g);
    drop(local);
    local::remove(&prefix).unwrap();
}

#[test]
fn keys() {
    // Small encodings are the key itself.
    assert_eq!(
        spo_key(1, 2, 3),
        Uuid {
            uuid0: 0x03_02_01,
            uuid1: 0
        }
    );
    assert_eq!(spo_key(1, 2, 300).uuid0, 0x02_ac_02_01);
    // Large ones are hashed.
    let big = spo_key(u64::MAX, u64::MAX >> 1, 1 << 62);
    assert_eq!(big, spo_key(u64::MAX, u64::MAX >> 1, 1 << 62));
    assert_ne!(big, spo_key(u64::MAX, u64::MAX >> 1, 1 << 61));
    // Every key can be a table key.
    assert_ne!(spo_key(0, 0, 0).uuid0, 0);

    assert_eq!(intersection_key(5, 6), Uuid { uuid0: 5, uuid1: 6 });
    assert_eq!(Intersection::of(Linkage::Object, Linkage::Subject), None);
    assert_eq!(
        BuiltinIndex::for_linkage(Linkage::Author).table(),
        BuiltinIndex::Author as usize
    );
    assert_eq!(
        BuiltinIndex::for_linkage(Linkage::Subgraph),
        BuiltinIndex::Subgraph
    );
}
