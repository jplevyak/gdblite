use std::collections::VecDeque;

use super::*;
use crate::RecordLog;

/// A small deterministic generator so tests need no dependencies.
struct XorShift(u64);

impl XorShift {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }

    /// Ids skewed towards small values, all encodable.
    fn skewed(&mut self, n: usize) -> Vec<u64> {
        (0..n)
            .map(|_| {
                let shift = 1 + self.next() % 63;
                self.next() >> shift
            })
            .collect()
    }
}

/// A temporary directory removed on drop.
struct TempDir(PathBuf);

impl TempDir {
    fn new(name: &str) -> Self {
        let dir = std::env::temp_dir().join(format!("gdblite-rl-{}-{name}", std::process::id()));
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

/// Small files, so that a few hundred KiB spans several.
fn small_files() -> Options {
    Options {
        min_file_reserve: 1 << 16,
        ..Options::default()
    }
}

/// Pushes `n` values of varied sizes, returning their ids and data.
fn push_values(log: &mut LocalRecordLog, rng: &mut XorShift, n: usize) -> Vec<(u64, Vec<u8>)> {
    (0..n)
        .map(|_| {
            let len = (rng.next() % 3000) as usize;
            let data: Vec<u8> = (0..len).map(|i| (i as u64 ^ rng.0) as u8).collect();
            (log.push_value(&data).unwrap(), data)
        })
        .collect()
}

/// Checks that the log holds `values` in order, possibly with filler nodes
/// between them, and returns the filler ids.
fn check_values_and_filler(log: &LocalRecordLog, values: &[(u64, Vec<u8>)]) -> Vec<u64> {
    let mut filler = Vec::new();
    let mut expected = values.iter().peekable();
    for r in log.records(log.start_id()) {
        let r = r.unwrap();
        match expected.peek() {
            Some((id, data)) if *id == r.id() => {
                assert_eq!(r.data(), Some(&data[..]), "id {id}");
                expected.next();
            }
            _ => {
                assert!(r.is_node(), "id {} is not filler", r.id());
                filler.push(r.id());
            }
        }
    }
    assert!(expected.next().is_none());
    filler
}

/// Checks that the log holds exactly `values`.
fn check_values(log: &LocalRecordLog, values: &[(u64, Vec<u8>)]) {
    assert_eq!(check_values_and_filler(log, values), Vec::<u64>::new());
}

const SIMPLE_SIZE: usize = 10_000;
const LINKAGES_PER_TUPLE: usize = 10;

#[test]
fn simple() {
    let dir = TempDir::new("simple");
    let prefix = dir.prefix("simple");
    let mut l = LocalRecordLog::open(Some(&prefix), Options::default()).unwrap();

    // Write tuples with random linkages.
    let ids = XorShift(0x2545_f491_4f6c_dd1d).skewed(SIMPLE_SIZE * LINKAGES_PER_TUPLE);
    let mut log = RecordLog::new(1);
    let records: Vec<u64> = ids
        .chunks(LINKAGES_PER_TUPLE)
        .map(|c| log.push_tuple(false, c).unwrap())
        .collect();
    for &id in &records {
        l.append(&Record::from_log(&log, id).unwrap()).unwrap();
    }
    assert_eq!(l.next_id(), log.end_id());

    // Read back 4 and check that we got the right ones.
    for r in [13, 2145, 3, 9765] {
        let record = l.get(records[r]).unwrap();
        let link = record.as_link().unwrap();
        assert_eq!(link.len(), 10);
        for j in 0..5 {
            assert_eq!(link.field(j), ids[r * LINKAGES_PER_TUPLE + j]);
        }
        assert_eq!(
            link.subgraphs(),
            ids[r * LINKAGES_PER_TUPLE + 5..(r + 1) * LINKAGES_PER_TUPLE]
        );
    }

    // Read back a range which ends inside an entry, and decode it.
    let mut start = records[13];
    for (id, bytes) in l.range(records[13], records[2145] + 1).unwrap() {
        assert_eq!(id, start);
        let mut pos = 0;
        while let Ok(r) = Record::from_bytes(start, &bytes[pos..]) {
            pos += r.bytes().len();
            start = r.next_id();
        }
    }
    assert_eq!(start, records[2145]);
}

#[test]
fn transient() {
    let mut log = LocalRecordLog::open(
        None,
        Options {
            start_id: 100,
            ..small_files()
        },
    )
    .unwrap();
    assert!(log.is_transient());
    assert_eq!((log.start_id(), log.next_id()), (100, 100));
    let n = log.push_node().unwrap();
    let v = log.push_value(b"hello").unwrap();
    let t = log.push_tuple(true, &[n, v]).unwrap();
    assert_eq!(n, 100);
    assert!(log.get(n).unwrap().is_node());
    assert_eq!(log.get(v).unwrap().data(), Some(&b"hello"[..]));
    assert_eq!(log.get(t).unwrap().as_link().unwrap().ids(), [n, v]);
    assert!(log.get(99).is_err());
    assert!(log.get(log.next_id()).is_err());
    assert!(log.range(99, 101).is_err());
    assert!(log.range(100, log.next_id() + 1).is_err());
    assert_eq!(log.range(v, v).unwrap().count(), 0);
    log.commit().unwrap();

    let mut rng = XorShift(7);
    let mut log = LocalRecordLog::open(None, small_files()).unwrap();
    let values = push_values(&mut log, &mut rng, 500);
    assert!(log.file_count() > 3);
    check_values(&log, &values);
}

#[test]
fn append_checks_id() {
    let mut log = LocalRecordLog::open(None, Options::default()).unwrap();
    let r = Record::value(5, b"x");
    assert!(matches!(log.append(&r), Err(Error::InvalidArgument(_))));
    log.append(&r.with_id(1).unwrap()).unwrap();
    log.append(&Record::tuple(log.next_id(), false, &[1]).unwrap())
        .unwrap();
    assert_eq!(
        log.get(log.records(1).nth(1).unwrap().unwrap().id())
            .unwrap()
            .as_link()
            .unwrap()
            .ids(),
        [1]
    );
}

#[test]
fn reopen() {
    let dir = TempDir::new("reopen");
    let prefix = dir.prefix("log");
    assert!(!exists(&prefix));
    let mut rng = XorShift(11);
    let uuid;
    let mut values;
    let committed_next;
    let crash_next;
    {
        let mut log = LocalRecordLog::open(Some(&prefix), small_files()).unwrap();
        assert!(exists(&prefix));
        uuid = log.metadata().uuid;
        assert_ne!(uuid, Uuid::default());
        values = push_values(&mut log, &mut rng, 300);
        log.commit().unwrap();
        assert!(log.file_count() > 1);

        // A second opener is locked out.
        let err = LocalRecordLog::open(Some(&prefix), small_files())
            .err()
            .unwrap();
        assert!(matches!(err, Error::Io(e) if e.kind() == io::ErrorKind::WouldBlock));

        // Uncommitted entries are lost, but their ids are not reused: they
        // become filler nodes, as do the other reserved ids.
        committed_next = log.next_id();
        push_values(&mut log, &mut rng, 300);
        crash_next = log.next_id();
    }
    let mut log = LocalRecordLog::open(Some(&prefix), small_files()).unwrap();
    assert_eq!(log.metadata().uuid, uuid);
    assert_eq!(log.next_id(), log.metadata().next_id);
    assert!(log.next_id() >= crash_next);
    assert_eq!(log.metadata().reserved_id, log.next_id());
    let filler = check_values_and_filler(&log, &values);
    assert_eq!(filler, (committed_next..log.next_id()).collect::<Vec<_>>());

    values.extend(push_values(&mut log, &mut rng, 300));
    log.commit().unwrap();
    drop(log);
    let log = LocalRecordLog::open(Some(&prefix), small_files()).unwrap();
    assert_eq!(check_values_and_filler(&log, &values), filler);
    drop(log);

    remove(&prefix).unwrap();
    assert!(!exists(&prefix));
    assert!(!file_path(&prefix, 0).exists());
}

#[test]
fn large_entry() {
    let mut log = LocalRecordLog::open(None, small_files()).unwrap();
    let a = log.push_value(b"a").unwrap();
    let big = vec![7u8; 300_000];
    let b = log.push_value(&big).unwrap();
    let c = log.push_value(b"c").unwrap();
    assert_eq!(log.get(a).unwrap().data(), Some(&b"a"[..]));
    assert_eq!(log.get(b).unwrap().data(), Some(&big[..]));
    assert_eq!(log.get(c).unwrap().data(), Some(&b"c"[..]));
    assert_eq!(
        log.range(a, log.next_id()).unwrap().count(),
        log.file_count()
    );
}

#[test]
fn interrupted_commit() {
    let dir = TempDir::new("interrupted");
    let prefix = dir.prefix("log");
    let mut rng = XorShift(13);
    let open = || LocalRecordLog::open(Some(&prefix), small_files()).unwrap();

    let mut log = open();
    let committed = push_values(&mut log, &mut rng, 100);
    log.commit().unwrap();
    let pending = push_values(&mut log, &mut rng, 300);
    log.start_commit().unwrap();
    drop(log);

    // Completing the commit keeps the pending entries, and fills the rest of
    // the reserved ids.
    let mut log = open();
    assert!(log.has_pending_commit());
    let reserved = log.metadata().reserved_id;
    assert_eq!(log.next_id(), pending[0].0);
    assert!(log.get(pending[0].0).is_err());
    assert!(matches!(log.push_node(), Err(Error::InvalidArgument(_))));
    assert!(matches!(log.start_commit(), Err(Error::InvalidArgument(_))));
    log.complete_commit().unwrap();
    assert!(!log.has_pending_commit());
    assert_eq!(log.next_id(), reserved);
    let all: Vec<_> = committed.iter().chain(&pending).cloned().collect();
    let pending_end = log.get(pending.last().unwrap().0).unwrap().next_id();
    let mut filler = check_values_and_filler(&log, &all);
    assert_eq!(filler, (pending_end..reserved).collect::<Vec<_>>());
    let more = push_values(&mut log, &mut rng, 300);
    log.start_commit().unwrap();
    drop(log);

    // Abandoning it makes them filler too.
    let mut log = open();
    assert!(log.has_pending_commit());
    log.abandon_commit().unwrap();
    assert!(!log.has_pending_commit());
    assert_eq!(log.next_id(), log.metadata().reserved_id);
    assert!(more.iter().all(|(id, _)| log.get(*id).unwrap().is_node()));
    filler.extend(more[0].0..log.next_id());
    assert_eq!(check_values_and_filler(&log, &all), filler);
    let again = push_values(&mut log, &mut rng, 10);
    assert_eq!(again[0].0, filler.last().unwrap() + 1);
    log.commit().unwrap();
    drop(log);
    let all: Vec<_> = all.into_iter().chain(again).collect();
    assert_eq!(check_values_and_filler(&open(), &all), filler);

    // Abandoning appends while open does not reuse their ids either.
    let mut log = open();
    let end = log.next_id();
    let dropped = log.push_value(b"dropped").unwrap();
    log.abandon_commit().unwrap();
    assert_eq!(dropped, end);
    assert!(log.get(dropped).unwrap().is_node());
    assert!(log.push_value(b"kept").unwrap() > dropped);
}

fn snapshot_chunks(log: &LocalRecordLog) -> VecDeque<Vec<u8>> {
    let mut chunks = VecDeque::new();
    log.snapshot(|c| {
        chunks.push_back(c.to_vec());
        Ok(())
    })
    .unwrap();
    chunks
}

fn source(mut chunks: VecDeque<Vec<u8>>) -> impl FnMut() -> io::Result<Vec<u8>> {
    move || Ok(chunks.pop_front().unwrap_or_default())
}

#[test]
fn snapshot_restore() {
    let dir = TempDir::new("snapshot");
    let prefix = dir.prefix("log");
    let mut rng = XorShift(17);
    let mut log = LocalRecordLog::open(
        Some(&prefix),
        Options {
            start_id: 50,
            ..small_files()
        },
    )
    .unwrap();
    let values = push_values(&mut log, &mut rng, 400);
    assert!(log.file_count() > 3);
    let chunks = snapshot_chunks(&log);

    // Into a new persistent log, as one file.
    let copy = dir.prefix("copy");
    let restored =
        LocalRecordLog::restore(Some(&copy), small_files(), source(chunks.clone())).unwrap();
    assert_eq!(restored.file_count(), 1);
    assert_eq!(restored.metadata().uuid, log.metadata().uuid);
    assert_eq!(restored.start_id(), 50);
    check_values(&restored, &values);
    drop(restored);
    let mut reopened = LocalRecordLog::open(Some(&copy), small_files()).unwrap();
    check_values(&reopened, &values);
    let id = reopened.push_value(b"after").unwrap();
    assert_eq!(id, log.next_id());

    // Not over an existing log.
    let err = LocalRecordLog::restore(Some(&copy), small_files(), source(chunks.clone()));
    assert!(matches!(err, Err(Error::Io(e)) if e.kind() == io::ErrorKind::AlreadyExists));

    // Into memory, with chunks split differently.
    let bytes: Vec<u8> = chunks.iter().skip(1).flatten().copied().collect();
    let mut rechunked: VecDeque<Vec<u8>> = bytes.chunks(777).map(<[u8]>::to_vec).collect();
    rechunked.push_front(chunks[0].clone());
    let restored = LocalRecordLog::restore(None, Options::default(), source(rechunked)).unwrap();
    check_values(&restored, &values);

    // A truncated snapshot fails and leaves nothing behind.
    let mut truncated = chunks.clone();
    truncated.pop_back();
    let bad = dir.prefix("bad");
    let err = LocalRecordLog::restore(Some(&bad), small_files(), source(truncated));
    assert!(matches!(err, Err(Error::InvalidFormat(_))));
    assert!(!exists(&bad));
    assert!(!file_path(&bad, 0).exists());

    let mut corrupt = chunks;
    corrupt[0][0] ^= 1;
    let err = LocalRecordLog::restore(None, small_files(), source(corrupt));
    assert!(matches!(err, Err(Error::InvalidFormat(_))));
}

#[test]
fn invalid_files() {
    let dir = TempDir::new("invalid");
    let prefix = dir.prefix("log");
    let mut log = LocalRecordLog::open(Some(&prefix), small_files()).unwrap();
    push_values(&mut log, &mut XorShift(19), 10);
    log.commit().unwrap();
    drop(log);

    fs::remove_file(file_path(&prefix, 0)).unwrap();
    let err = LocalRecordLog::open(Some(&prefix), small_files())
        .err()
        .unwrap();
    assert!(matches!(err, Error::InvalidFormat(_)));

    fs::write(suffixed(&prefix, "metadata"), [1u8; 10]).unwrap();
    let err = LocalRecordLog::open(Some(&prefix), small_files())
        .err()
        .unwrap();
    assert!(matches!(err, Error::InvalidFormat(_)));
}
