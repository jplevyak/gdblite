//! The record format stored in dynamic table buckets.
//!
//! A bucket is 128 bytes: a 4 byte little endian header followed by records
//! packed back to back.  Every byte of a record is non-zero and every byte after
//! the last record is zero, so a record that is only partly written (by a writer
//! that crashed, or one a reader observes mid-append) shows up as zero bytes
//! where the rest of it belongs.
//!
//! A record is a header byte, then the key as an optional varint `key2` and a
//! varint `key` unless the record reuses the key of the record before it in the
//! same bucket, then its values as varints.  The header byte is
//!
//! ```text
//! bits 0-2  kind: 2 insert, 3 replace, 4 delete; 5-7 the same reusing the previous key
//! bit  3    extra bit
//! bit  4    a 128 bit key (key2 != 0)
//! bits 5-7  number of values - 1, or zero for a delete, which has no values
//! ```
//!
//! Varints are LEB128.  The encoding of a non-zero integer never contains a zero
//! byte, which is why keys and values must be non-zero.

pub const BUCKET_BYTES: usize = 128;
pub const BUCKET_HEADER_BYTES: usize = 4;
/// The most values one record holds.
pub const MAX_VALUES: usize = 8;
/// The largest encoded record: header, key2, key and 8 values.
pub const MAX_RECORD_BYTES: usize = 1 + 10 * (2 + MAX_VALUES);

const PREVIOUS: u8 = 3;
const EXTRA_BIT: u8 = 1 << 3;
const KEY2_BIT: u8 = 1 << 4;
const VALUES_SHIFT: u32 = 5;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    Insert,
    Replace,
    Delete,
}

impl Kind {
    fn code(self) -> u8 {
        match self {
            Kind::Insert => 2,
            Kind::Replace => 3,
            Kind::Delete => 4,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Record {
    pub kind: Kind,
    pub bit: bool,
    pub key: u64,
    pub key2: u64,
    len: usize,
    values: [u64; MAX_VALUES],
}

impl Record {
    pub fn values(&self) -> &[u64] {
        &self.values[..self.len]
    }
}

fn put_varint(out: &mut [u8], pos: &mut usize, mut v: u64) {
    while v >= 0x80 {
        out[*pos] = v as u8 | 0x80;
        v >>= 7;
        *pos += 1;
    }
    out[*pos] = v as u8;
    *pos += 1;
}

/// Decodes a varint, rejecting zero bytes (a missing or corrupt byte).
fn get_varint(bytes: &[u8], pos: &mut usize) -> Option<u64> {
    let mut v = 0u64;
    let mut shift = 0;
    loop {
        let b = *bytes.get(*pos)?;
        if b == 0 {
            return None;
        }
        *pos += 1;
        let bits = u64::from(b & 0x7f);
        if shift >= 64 || (shift > 57 && bits >> (64 - shift) != 0) {
            return None;
        }
        v |= bits << shift;
        if b & 0x80 == 0 {
            return Some(v);
        }
        shift += 7;
    }
}

/// Encodes a record into `out`, returning its length.  With `previous` the key
/// is left out and taken from the record before it.
pub fn encode(
    out: &mut [u8; MAX_RECORD_BYTES],
    kind: Kind,
    previous: bool,
    bit: bool,
    key: u64,
    key2: u64,
    values: &[u64],
) -> usize {
    debug_assert!(key != 0 && !values.contains(&0));
    debug_assert!(if kind == Kind::Delete {
        values.is_empty()
    } else {
        (1..=MAX_VALUES).contains(&values.len())
    });
    let mut header = kind.code() + if previous { PREVIOUS } else { 0 };
    if bit {
        header |= EXTRA_BIT;
    }
    if key2 != 0 {
        header |= KEY2_BIT;
    }
    if let Some(n) = values.len().checked_sub(1) {
        header |= (n as u8) << VALUES_SHIFT;
    }
    out[0] = header;
    let mut pos = 1;
    if !previous {
        if key2 != 0 {
            put_varint(out, &mut pos, key2);
        }
        put_varint(out, &mut pos, key);
    }
    for &v in values {
        put_varint(out, &mut pos, v);
    }
    pos
}

/// Parses the record at `pos`, returning it and the position after it, or
/// `None` if it is incomplete or corrupt.  `last` is the key of the record
/// before it in the bucket.
fn parse_record(bytes: &[u8], pos: usize, last: Option<(u64, u64)>) -> Option<(Record, usize)> {
    let header = bytes[pos];
    let (kind, previous) = match header & 7 {
        2 => (Kind::Insert, false),
        3 => (Kind::Replace, false),
        4 => (Kind::Delete, false),
        5 => (Kind::Insert, true),
        6 => (Kind::Replace, true),
        7 => (Kind::Delete, true),
        _ => return None,
    };
    let count = usize::from(header >> VALUES_SHIFT);
    let len = match kind {
        Kind::Delete if count != 0 => return None,
        Kind::Delete => 0,
        _ => count + 1,
    };
    let has_key2 = header & KEY2_BIT != 0;
    let mut p = pos + 1;
    let (key, key2) = if previous {
        let (key, key2) = last?;
        if has_key2 != (key2 != 0) {
            return None;
        }
        (key, key2)
    } else {
        let key2 = if has_key2 {
            get_varint(bytes, &mut p)?
        } else {
            0
        };
        (get_varint(bytes, &mut p)?, key2)
    };
    let mut values = [0u64; MAX_VALUES];
    for v in &mut values[..len] {
        *v = get_varint(bytes, &mut p)?;
    }
    let record = Record {
        kind,
        bit: header & EXTRA_BIT != 0,
        key,
        key2,
        len,
        values,
    };
    Some((record, p))
}

/// The result of parsing a bucket.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Parsed {
    /// The position after the last complete record.
    pub end: usize,
    /// Parsing stopped at an incomplete or corrupt record.
    pub corrupt: bool,
    /// The key of the last complete record.
    pub last_key: Option<(u64, u64)>,
}

/// Calls `visit` on each complete record in a bucket, in order.
pub fn parse_bucket(bytes: &[u8; BUCKET_BYTES], mut visit: impl FnMut(&Record)) -> Parsed {
    let mut pos = BUCKET_HEADER_BYTES;
    let mut last_key = None;
    while pos < BUCKET_BYTES && bytes[pos] != 0 {
        let Some((record, next)) = parse_record(bytes, pos, last_key) else {
            return Parsed {
                end: pos,
                corrupt: true,
                last_key,
            };
        };
        last_key = Some((record.key, record.key2));
        visit(&record);
        pos = next;
    }
    Parsed {
        end: pos,
        corrupt: false,
        last_key,
    }
}

pub fn bucket_header(bytes: &[u8; BUCKET_BYTES]) -> u32 {
    u32::from_le_bytes(bytes[..BUCKET_HEADER_BYTES].try_into().unwrap())
}

/// Applies a record to a key's values and extra bit.
pub fn merge(values: &mut Vec<u64>, bit: &mut bool, record: &Record) {
    match record.kind {
        Kind::Insert => {}
        Kind::Replace | Kind::Delete => values.clear(),
    }
    values.extend_from_slice(record.values());
    if record.kind != Kind::Delete {
        *bit = record.bit;
    }
}

/// A key's state, accumulated from its records.
#[derive(Debug, Default)]
pub struct KeyState {
    pub bit: bool,
    pub values: Vec<u64>,
    last: Option<Record>,
}

impl KeyState {
    pub fn apply(&mut self, record: &Record) {
        merge(&mut self.values, &mut self.bit, record);
        self.last = Some(record.clone());
    }

    /// Whether applying an operation would change nothing: deleting a missing
    /// key, or repeating the key's most recent operation.  Skipping these makes
    /// replaying an operation after a crash idempotent.
    pub fn is_noop(&self, kind: Kind, bit: bool, values: &[u64]) -> bool {
        match kind {
            Kind::Delete => self.values.is_empty(),
            _ => self
                .last
                .as_ref()
                .is_some_and(|r| r.kind == kind && r.bit == bit && r.values() == values),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn bucket_with(records: &[(Kind, bool, u64, u64, &[u64])]) -> [u8; BUCKET_BYTES] {
        let mut bucket = [0u8; BUCKET_BYTES];
        let mut pos = BUCKET_HEADER_BYTES;
        let mut last = None;
        for &(kind, bit, key, key2, values) in records {
            let mut rec = [0u8; MAX_RECORD_BYTES];
            let n = encode(
                &mut rec,
                kind,
                last == Some((key, key2)),
                bit,
                key,
                key2,
                values,
            );
            bucket[pos..pos + n].copy_from_slice(&rec[..n]);
            pos += n;
            last = Some((key, key2));
        }
        bucket
    }

    fn parse_all(bucket: &[u8; BUCKET_BYTES]) -> (Vec<Record>, Parsed) {
        let mut records = Vec::new();
        let parsed = parse_bucket(bucket, |r| records.push(r.clone()));
        (records, parsed)
    }

    #[test]
    fn round_trip() {
        let bucket = bucket_with(&[
            (Kind::Insert, true, 1234, 0, &[12, 1313131313]),
            (Kind::Insert, false, 1234, 0, &[u64::MAX]),
            (Kind::Delete, false, 77, 5678, &[]),
            (Kind::Replace, true, 77, 5678, &[1, 2, 3, 4, 5, 6, 7, 8]),
        ]);
        let (records, parsed) = parse_all(&bucket);
        assert!(!parsed.corrupt);
        assert_eq!(parsed.last_key, Some((77, 5678)));
        assert_eq!(records.len(), 4);
        assert_eq!(
            (records[1].key, records[1].values()),
            (1234, &[u64::MAX][..])
        );
        assert_eq!((records[2].kind, records[2].key2), (Kind::Delete, 5678));
        assert!(records[2].values().is_empty());
        assert_eq!(records[3].values(), &[1, 2, 3, 4, 5, 6, 7, 8]);
        assert!(bucket[parsed.end..].iter().all(|&b| b == 0));
    }

    #[test]
    fn delete_does_not_swallow_later_records() {
        let bucket = bucket_with(&[
            (Kind::Insert, false, 1, 0, &[10]),
            (Kind::Delete, false, 1, 0, &[]),
            (Kind::Insert, false, 2, 0, &[20]),
        ]);
        let (records, parsed) = parse_all(&bucket);
        assert!(!parsed.corrupt);
        assert_eq!(records.len(), 3);
        assert_eq!((records[2].key, records[2].values()), (2, &[20][..]));
    }

    #[test]
    fn any_truncation_is_detected() {
        let bucket = bucket_with(&[
            (Kind::Insert, false, 1, 0, &[10]),
            (Kind::Insert, true, 300, 99, &[1 << 40, 5]),
        ]);
        let (_, full) = parse_all(&bucket);
        let second = 4 + 3;
        for cut in second + 1..full.end {
            let mut partial = bucket;
            partial[cut..].fill(0);
            let (records, parsed) = parse_all(&partial);
            assert_eq!(records.len(), 1, "cut at {cut}");
            assert!(parsed.corrupt);
            assert_eq!(parsed.end, second);
        }
    }

    #[test]
    fn previous_at_bucket_start_is_corrupt() {
        let mut bucket = [0u8; BUCKET_BYTES];
        let mut rec = [0u8; MAX_RECORD_BYTES];
        let n = encode(&mut rec, Kind::Insert, true, false, 1, 0, &[1]);
        bucket[4..4 + n].copy_from_slice(&rec[..n]);
        assert!(parse_all(&bucket).1.corrupt);
    }

    #[test]
    fn key_state() {
        let mut s = KeyState::default();
        let bucket = bucket_with(&[
            (Kind::Insert, true, 1, 0, &[1, 2]),
            (Kind::Insert, false, 1, 0, &[3]),
        ]);
        parse_bucket(&bucket, |r| s.apply(r));
        assert_eq!((s.bit, &s.values[..]), (false, &[1, 2, 3][..]));
        assert!(s.is_noop(Kind::Insert, false, &[3]));
        assert!(!s.is_noop(Kind::Insert, false, &[1, 2]));
        assert!(!s.is_noop(Kind::Replace, false, &[3]));
        assert!(!s.is_noop(Kind::Delete, false, &[]));
        assert!(KeyState::default().is_noop(Kind::Delete, false, &[]));
    }
}
