use std::collections::BTreeMap;
use std::fs;

use super::*;
use crate::local_table::IdSet;
use crate::record::{LinkFields, Record, Uuid};

/// A small deterministic generator so tests need no dependencies.
struct XorShift(u64);

impl XorShift {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
}

/// A temporary directory removed on drop.
struct TempDir(PathBuf);

impl TempDir {
    fn new(name: &str) -> Self {
        let dir = std::env::temp_dir().join(format!("gdblite-local-{}-{name}", std::process::id()));
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

const SUBJECTS: usize = 0;
const OBJECTS: usize = 1;
const VALUES: usize = 2;

fn three_indexes() -> Options {
    Options {
        tables: 3,
        ..Options::default()
    }
}

/// What a populated database should hold.
#[derive(Default)]
struct Expected {
    records: Vec<(u64, Vec<u8>)>,
    subjects: BTreeMap<u64, IdSet>,
    objects: BTreeMap<u64, IdSet>,
    values: Vec<(Uuid, u64)>,
}

/// Appends `n` values, each the object of a link from a random earlier value,
/// indexing the links by subject and object and the values by uuid.
fn populate(local: &mut Local, rng: &mut XorShift, n: usize, expected: &mut Expected) {
    for _ in 0..n {
        let data = format!("value {}", rng.next() % 1_000_000);
        let id = local.log_mut().push_value(data.as_bytes()).unwrap();
        let uuid = Uuid::of(data.as_bytes());
        local.table_mut(VALUES).set(uuid, id).unwrap();
        expected.values.push((uuid, id));
        expected
            .records
            .push((id, local.log().get(id).unwrap().bytes().to_vec()));

        let subject = expected.values[(rng.next() % expected.values.len() as u64) as usize].1;
        let link = Record::link(
            local.log().next_id(),
            &LinkFields {
                subject,
                predicate: expected.values[0].1,
                object: id,
                when: 1_700_000_000_000 + rng.next() % 1000,
                ..LinkFields::default()
            },
        )
        .unwrap();
        local.log_mut().append(&link).unwrap();
        local
            .table_mut(SUBJECTS)
            .insert(subject, [link.id()])
            .unwrap();
        local.table_mut(OBJECTS).insert(id, [link.id()]).unwrap();
        expected
            .subjects
            .entry(subject)
            .or_default()
            .insert(link.id());
        expected.objects.entry(id).or_default().insert(link.id());
        expected.records.push((link.id(), link.bytes().to_vec()));
    }
}

fn check(local: &Local, expected: &Expected) {
    let records: Vec<(u64, Vec<u8>)> = local
        .log()
        .records(local.log().start_id())
        .map(|r| {
            let r = r.unwrap();
            (r.id(), r.bytes().to_vec())
        })
        .collect();
    assert_eq!(records, expected.records);
    for (key, set) in &expected.subjects {
        assert_eq!(&local.table(SUBJECTS).find(*key), set);
    }
    for (key, set) in &expected.objects {
        assert_eq!(&local.table(OBJECTS).find(*key), set);
    }
    for (uuid, _) in &expected.values {
        // Repeated data maps to its last value.
        let last = expected
            .values
            .iter()
            .rev()
            .find(|(u, _)| u == uuid)
            .unwrap()
            .1;
        assert_eq!(local.table(VALUES).find_one(*uuid), Some(last));
    }
}

#[test]
fn transient() {
    let mut local = Local::open(None, three_indexes()).unwrap();
    assert!(local.is_transient());
    assert_eq!(local.table_count(), 3);
    let mut expected = Expected::default();
    populate(&mut local, &mut XorShift(1), 500, &mut expected);
    local.commit().unwrap();
    local.tick().unwrap();
    check(&local, &expected);
    assert_eq!(local.new_table().unwrap(), 3);
    local.table_mut(3).insert(1u64, [2]).unwrap();
    assert_eq!(local.table(3).find(1u64), [2].into());
}

#[test]
fn reopen() {
    let dir = TempDir::new("reopen");
    let prefix = dir.prefix("db");
    assert!(!exists(&prefix));
    let mut rng = XorShift(2);
    let mut expected = Expected::default();
    {
        let mut local = Local::open(Some(&prefix), three_indexes()).unwrap();
        assert!(exists(&prefix));
        assert_eq!(local.prefix(), Some(prefix.as_path()));
        populate(&mut local, &mut rng, 500, &mut expected);
        local.commit().unwrap();
        assert_eq!(local.new_table().unwrap(), 3);
        local.table_mut(3).set(5u64, 6).unwrap();
        local.commit().unwrap();
    }
    // The indexes are found without being asked for.
    let mut local = Local::open(Some(&prefix), Options::default()).unwrap();
    assert_eq!(local.table_count(), 4);
    assert_eq!(local.table(3).find_one(5u64), Some(6));
    check(&local, &expected);

    populate(&mut local, &mut rng, 100, &mut expected);
    local.commit().unwrap();
    drop(local);
    check(
        &Local::open(Some(&prefix), Options::default()).unwrap(),
        &expected,
    );

    remove(&prefix).unwrap();
    assert!(!exists(&prefix));
    assert!(!local_table::exists(&index_prefix(&prefix, 0)));
    assert!(!local_table::exists(&index_prefix(&prefix, 3)));
}

#[test]
fn uncommitted_index_entries_name_filler() {
    let dir = TempDir::new("uncommitted");
    let prefix = dir.prefix("db");
    let mut expected = Expected::default();
    let lost;
    {
        let mut local = Local::open(Some(&prefix), three_indexes()).unwrap();
        populate(&mut local, &mut XorShift(3), 50, &mut expected);
        local.commit().unwrap();
        // An entry and its index entry, never committed.
        lost = local.log_mut().push_value(b"lost").unwrap();
        local
            .table_mut(VALUES)
            .set(Uuid::of(b"lost"), lost)
            .unwrap();
    }
    let mut local = Local::open(Some(&prefix), Options::default()).unwrap();
    // The index entry survived, but names a node rather than any new entry.
    assert_eq!(local.table(VALUES).find_one(Uuid::of(b"lost")), Some(lost));
    assert!(local.log().get(lost).unwrap().is_node());
    let id = local.log_mut().push_value(b"new").unwrap();
    assert!(id > lost);
    assert!(local.log().get(lost).unwrap().is_node());
}

fn snapshot_bytes(local: &mut Local) -> Vec<u8> {
    let mut bytes = Vec::new();
    local.snapshot(&mut bytes).unwrap();
    bytes
}

#[test]
fn snapshot() {
    let dir = TempDir::new("snapshot");
    let prefix = dir.prefix("db");
    let mut local = Local::open(Some(&prefix), three_indexes()).unwrap();
    let mut expected = Expected::default();
    populate(&mut local, &mut XorShift(4), 2000, &mut expected);
    local.commit().unwrap();
    let bytes = snapshot_bytes(&mut local);
    drop(local);

    let copy = dir.prefix("copy");
    let restored = Local::restore(Some(&copy), Options::default(), &mut &bytes[..]).unwrap();
    assert_eq!(restored.table_count(), 3);
    check(&restored, &expected);
    drop(restored);
    check(
        &Local::open(Some(&copy), Options::default()).unwrap(),
        &expected,
    );

    // Not over an existing database.
    let err = Local::restore(Some(&copy), Options::default(), &mut &bytes[..]);
    assert!(matches!(err, Err(Error::Io(e)) if e.kind() == io::ErrorKind::AlreadyExists));

    // A truncated or extended snapshot fails, leaving nothing behind.
    let bad = dir.prefix("bad");
    let err = Local::restore(
        Some(&bad),
        Options::default(),
        &mut &bytes[..bytes.len() - 1],
    );
    assert!(err.is_err());
    assert!(!exists(&bad));
    assert!(!local_table::exists(&index_prefix(&bad, 0)));
    let mut extended = bytes.clone();
    extended.push(0);
    let err = Local::restore(Some(&bad), Options::default(), &mut &extended[..]);
    assert!(matches!(err, Err(Error::InvalidFormat(_))));
    assert!(!exists(&bad));
    let mut corrupt = bytes;
    corrupt[0] ^= 1;
    let err = Local::restore(None, Options::default(), &mut &corrupt[..]);
    assert!(matches!(err, Err(Error::InvalidFormat(_))));
}

#[test]
fn transient_snapshot() {
    let mut local = Local::open(None, three_indexes()).unwrap();
    let mut expected = Expected::default();
    let mut rng = XorShift(5);
    populate(&mut local, &mut rng, 1000, &mut expected);
    // Snapshots include entries not yet committed.
    let bytes = snapshot_bytes(&mut local);
    drop(local);

    let mut restored = Local::restore(None, three_indexes(), &mut &bytes[..]).unwrap();
    assert!(restored.is_transient());
    check(&restored, &expected);
    populate(&mut restored, &mut rng, 10, &mut expected);
    restored.commit().unwrap();
    check(&restored, &expected);

    let dir = TempDir::new("transient-snapshot");
    let prefix = dir.prefix("db");
    let restored = Local::restore(Some(&prefix), Options::default(), &mut &bytes[..]).unwrap();
    assert!(!restored.is_transient());
    drop(restored);
    let local = Local::open(Some(&prefix), Options::default()).unwrap();
    assert_eq!(local.log().records(local.log().start_id()).count(), 2000);
}

#[test]
fn sizing() {
    let options = Options::for_tuples(1000);
    assert_eq!(options.log.create_size, 1000 * LOG_BYTES_PER_TUPLE);
    assert_eq!(options.index.create_size, 1000 * INDEX_BYTES_PER_TUPLE);
    let local = Local::open(None, options).unwrap();
    assert_eq!(local.table_count(), 0);
}
