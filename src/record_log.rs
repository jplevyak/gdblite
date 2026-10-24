//! A log of tuples and values.
//!
//! A record log is a 16 byte [`PersistentHeader`] followed by a body of
//! back-to-back entries.  Each entry is identified by its *id*: its position
//! in the body plus the log's `start_id`, so the first entry of a log has id
//! `start_id` and the next entry begins where the previous one ends.
//!
//! The first bit of every entry says what it is:
//!
//! * **Value** (`0`): a [tagged variable integer](crate::coder) byte length
//!   followed by that many bytes.  A zero length value is a bare node.
//! * **Tuple** (`1`): a tagged variable integer count `n` followed by the `n`
//!   ids in [groups](crate::coder) of five, the last group holding the
//!   remainder.  Each id is [encoded](crate::coder::encode_field) relative to a
//!   base, normally the tuple's own id, so references to nearby earlier entries
//!   stay small.  The first group also carries a free *extra bit*.

use crate::coder::{
    GROUP_MAX, decode_field, decode_group, decode_tagged_varint, encode_field, encode_group,
    encode_tagged_varint, group_field, group_len, tagged_varint_len,
};
use crate::{Error, Result};

/// The current record log version.
pub const VERSION: u8 = 0;
/// The `written` header value for a restorable log.
pub const WRITTEN_VERSION: u8 = 1;
/// Identifies a record log.
pub const MAGIC: u32 = 0xe3e0_ba2f;
/// The size of the [`PersistentHeader`] in bytes.
pub const HEADER_SIZE: usize = 16;
/// The index of the WHEN (timestamp) field of a tuple, which is delta encoded.
pub const WHEN_FIELD: usize = 4;

/// The tag bit marking an entry as a tuple.
const TUPLE_TAG: u8 = 1;
const TAG_BITS: u32 = 1;

/// Compression applied to a record log body.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Compression {
    None = 0,
    V1 = 1,
}

impl Compression {
    pub const LAST: Compression = Compression::V1;
}

/// The header stored at the front of a record log.
///
/// Its physical layout is part of the format: these fields in this order,
/// little endian, 16 bytes in all.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PersistentHeader {
    pub magic: u32,
    /// Record log version.
    pub version: u8,
    /// Compression type/version, 0 is uncompressed.
    pub compression: u8,
    /// Write type/version, 0 = unwritten, 1+ = restorable.
    pub written: u8,
    /// Reserved for future use: should be zero.
    pub reserved: u8,
    /// The first id; 1 for a restorable root database.
    pub start_id: u64,
}

impl PersistentHeader {
    /// The default header for a log whose first entry will have `start_id`.
    pub fn new(start_id: u64) -> Self {
        PersistentHeader {
            magic: MAGIC,
            version: VERSION,
            compression: Compression::None as u8,
            written: 0,
            reserved: 0,
            start_id,
        }
    }

    pub fn is_valid(&self) -> bool {
        self.magic == MAGIC && self.version == VERSION
    }

    pub fn read(bytes: &[u8]) -> Result<Self> {
        let b: &[u8; HEADER_SIZE] = bytes
            .get(..HEADER_SIZE)
            .ok_or(Error::Truncated)?
            .try_into()
            .unwrap();
        Ok(PersistentHeader {
            magic: u32::from_le_bytes(b[0..4].try_into().unwrap()),
            version: b[4],
            compression: b[5],
            written: b[6],
            reserved: b[7],
            start_id: u64::from_le_bytes(b[8..16].try_into().unwrap()),
        })
    }

    pub fn to_bytes(&self) -> [u8; HEADER_SIZE] {
        let mut b = [0u8; HEADER_SIZE];
        b[0..4].copy_from_slice(&self.magic.to_le_bytes());
        b[4] = self.version;
        b[5] = self.compression;
        b[6] = self.written;
        b[7] = self.reserved;
        b[8..16].copy_from_slice(&self.start_id.to_le_bytes());
        b
    }

    pub fn write(&self, out: &mut [u8]) -> Result<()> {
        out.get_mut(..HEADER_SIZE)
            .ok_or(Error::BufferTooSmall)?
            .copy_from_slice(&self.to_bytes());
        Ok(())
    }
}

/// A value: a blob of bytes.  An empty value is a node.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Value<'a> {
    data: &'a [u8],
    encoded_len: usize,
}

impl<'a> Value<'a> {
    pub fn as_bytes(&self) -> &'a [u8] {
        self.data
    }

    pub fn len(&self) -> usize {
        self.data.len()
    }

    pub fn is_empty(&self) -> bool {
        self.data.is_empty()
    }

    /// The number of bytes the value occupies in the log.
    pub fn encoded_len(&self) -> usize {
        self.encoded_len
    }
}

/// A tuple: an ordered list of ids plus an extra bit.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Tuple<'a> {
    /// Exactly the encoded bytes of the tuple, already validated.
    raw: &'a [u8],
    header_len: usize,
    len: usize,
}

impl<'a> Tuple<'a> {
    /// The number of ids in the tuple.
    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// The number of bytes the tuple occupies in the log.
    pub fn encoded_len(&self) -> usize {
        self.raw.len()
    }

    pub fn extra_bit(&self) -> bool {
        self.len > 0 && self.raw[self.header_len] & 0x80 != 0
    }

    /// Calls `f(position, count)` for each group, stopping when `f` returns
    /// `Some`.
    fn find_group<T>(&self, mut f: impl FnMut(usize, usize) -> Option<T>) -> Option<T> {
        let mut pos = self.header_len;
        let mut remaining = self.len;
        while remaining > 0 {
            let count = remaining.min(GROUP_MAX);
            if let Some(t) = f(pos, count) {
                return Some(t);
            }
            pos += group_len(&self.raw[pos..], count).expect("validated tuple");
            remaining -= count;
        }
        None
    }

    /// The raw (still relative) encoding of id `i`.
    pub fn field(&self, i: usize) -> Option<u64> {
        if i >= self.len {
            return None;
        }
        let mut group_start = 0;
        self.find_group(|pos, count| {
            if i < group_start + count {
                Some(
                    group_field(&self.raw[pos..], count, i - group_start).expect("validated tuple"),
                )
            } else {
                group_start += count;
                None
            }
        })
    }

    /// Id `i`, decoded relative to `base`.
    pub fn id(&self, i: usize, base: u64) -> Option<u64> {
        self.field(i).map(|v| decode_field(base, v))
    }

    /// Appends the ids, decoded relative to `base`, to `out` and returns the
    /// extra bit.
    pub fn decode_into(&self, base: u64, out: &mut Vec<u64>) -> bool {
        let mut values = [0u64; GROUP_MAX];
        let mut extra = None;
        self.find_group(|pos, count| {
            let (e, _) =
                decode_group(&self.raw[pos..], &mut values[..count]).expect("validated tuple");
            extra.get_or_insert(e);
            out.extend(values[..count].iter().map(|&v| decode_field(base, v)));
            None::<()>
        });
        extra.unwrap_or(false)
    }

    /// The ids decoded relative to `base`, and the extra bit.
    pub fn decode(&self, base: u64) -> (Vec<u64>, bool) {
        let mut ids = Vec::with_capacity(self.len);
        let extra = self.decode_into(base, &mut ids);
        (ids, extra)
    }

    /// Conservatively estimates the encoded length of the tuple at the front of
    /// `buf`, which may be truncated.  Ids beyond `buf` are assumed to take 9
    /// bytes each.  Returns `None` if `buf` does not begin with a tuple header.
    pub fn estimate_len(buf: &[u8]) -> Option<usize> {
        let (tag, n, mut pos) = decode_tagged_varint(buf, TAG_BITS).ok()?;
        if tag != TUPLE_TAG {
            return None;
        }
        let limit = buf.len();
        let mut remaining = usize::try_from(n).unwrap_or(usize::MAX);
        while remaining > 0 {
            if pos + 2 > limit {
                return Some(pos.max(limit).saturating_add(remaining.saturating_mul(9)));
            }
            let count = remaining.min(GROUP_MAX);
            pos += group_len(&buf[pos..], count).ok()?;
            remaining -= count;
        }
        Some(pos)
    }
}

/// An entry in a record log.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Entry<'a> {
    Value(Value<'a>),
    Tuple(Tuple<'a>),
}

impl<'a> Entry<'a> {
    /// Parses the complete entry at the front of `buf`.
    pub fn parse(buf: &'a [u8]) -> Result<Self> {
        let (tag, n, header_len) = decode_tagged_varint(buf, TAG_BITS)?;
        let n = usize::try_from(n).map_err(|_| Error::Overflow)?;
        if tag != TUPLE_TAG {
            let end = header_len.checked_add(n).ok_or(Error::Overflow)?;
            let data = buf.get(header_len..end).ok_or(Error::Truncated)?;
            return Ok(Entry::Value(Value {
                data,
                encoded_len: end,
            }));
        }
        let mut pos = header_len;
        let mut remaining = n;
        while remaining > 0 {
            let count = remaining.min(GROUP_MAX);
            pos += group_len(buf.get(pos..).ok_or(Error::Truncated)?, count)?;
            remaining -= count;
        }
        let raw = buf.get(..pos).ok_or(Error::Truncated)?;
        Ok(Entry::Tuple(Tuple {
            raw,
            header_len,
            len: n,
        }))
    }

    /// The encoded length of the entry at the front of `buf`, or `None` if
    /// `buf` does not hold a complete entry.
    pub fn complete_len(buf: &[u8]) -> Option<usize> {
        Entry::parse(buf).ok().map(|e| e.encoded_len())
    }

    pub fn is_tuple(&self) -> bool {
        matches!(self, Entry::Tuple(_))
    }

    pub fn is_value(&self) -> bool {
        matches!(self, Entry::Value(_))
    }

    pub fn as_tuple(&self) -> Option<&Tuple<'a>> {
        match self {
            Entry::Tuple(t) => Some(t),
            Entry::Value(_) => None,
        }
    }

    pub fn as_value(&self) -> Option<&Value<'a>> {
        match self {
            Entry::Value(v) => Some(v),
            Entry::Tuple(_) => None,
        }
    }

    /// The number of bytes the entry occupies in the log.
    pub fn encoded_len(&self) -> usize {
        match self {
            Entry::Value(v) => v.encoded_len(),
            Entry::Tuple(t) => t.encoded_len(),
        }
    }
}

/// The largest number of bytes [`encode_value`] uses for `len` bytes of data.
pub fn max_value_len(len: usize) -> usize {
    tagged_varint_len(TAG_BITS, len as u64) + len
}

/// The largest number of bytes [`encode_tuple`] uses for `n` ids.
pub fn max_tuple_len(n: usize) -> usize {
    tagged_varint_len(TAG_BITS, n as u64) + 9 * n
}

/// Encodes a value holding `data` into the front of `out`, returning the
/// number of bytes written.
pub fn encode_value(out: &mut [u8], data: &[u8]) -> Result<usize> {
    let header_len = encode_tagged_varint(out, 0, TAG_BITS, data.len() as u64)?;
    let end = header_len + data.len();
    out.get_mut(header_len..end)
        .ok_or(Error::BufferTooSmall)?
        .copy_from_slice(data);
    Ok(end)
}

/// Encodes a tuple of `ids`, relative to `base`, into the front of `out`,
/// returning the number of bytes written.
pub fn encode_tuple(out: &mut [u8], base: u64, extra: bool, ids: &[u64]) -> Result<usize> {
    let mut pos = encode_tagged_varint(out, TUPLE_TAG, TAG_BITS, ids.len() as u64)?;
    let mut extra = extra;
    let mut fields = [0u64; GROUP_MAX];
    for group in ids.chunks(GROUP_MAX) {
        for (f, &id) in fields.iter_mut().zip(group) {
            *f = encode_field(base, id).ok_or(Error::Overflow)?;
        }
        pos += encode_group(&mut out[pos..], extra, &fields[..group.len()])?;
        extra = false;
    }
    Ok(pos)
}

/// A record log over a byte buffer.
///
/// Any `B: AsRef<[u8]>` can be read; `B: AsMut<[u8]>` can also be written in
/// place at caller-chosen ids, and `RecordLog<Vec<u8>>` can be appended to.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RecordLog<B> {
    buf: B,
}

impl<B: AsRef<[u8]>> RecordLog<B> {
    /// Wraps an existing log.  The header is not validated; see
    /// [`is_valid`](Self::is_valid).
    pub fn adopt(buf: B) -> Result<Self> {
        if buf.as_ref().len() < HEADER_SIZE {
            return Err(Error::Truncated);
        }
        Ok(RecordLog { buf })
    }

    pub fn into_inner(self) -> B {
        self.buf
    }

    /// The whole underlying buffer, header included.
    pub fn as_bytes(&self) -> &[u8] {
        self.buf.as_ref()
    }

    /// The log up to (not including) `next_id`, header included.
    pub fn bytes_until(&self, next_id: u64) -> Result<&[u8]> {
        let size = self.size(next_id).ok_or(Error::IdOutOfRange)?;
        self.as_bytes().get(..size).ok_or(Error::IdOutOfRange)
    }

    pub fn header(&self) -> PersistentHeader {
        PersistentHeader::read(self.as_bytes()).expect("adopted log has a header")
    }

    pub fn is_valid(&self) -> bool {
        self.header().is_valid()
    }

    pub fn start_id(&self) -> u64 {
        self.header().start_id
    }

    pub fn body(&self) -> &[u8] {
        &self.as_bytes()[HEADER_SIZE..]
    }

    /// The id following a log of `size` bytes, header included.
    pub fn next_id(&self, size: usize) -> u64 {
        self.start_id() + (size - HEADER_SIZE) as u64
    }

    /// The id just past the end of the buffer.
    pub fn end_id(&self) -> u64 {
        self.next_id(self.as_bytes().len())
    }

    /// The size in bytes, header included, of a log from `start_id` up to
    /// `next_id`.
    pub fn size_between(start_id: u64, next_id: u64) -> Option<usize> {
        let body = usize::try_from(next_id.checked_sub(start_id)?).ok()?;
        body.checked_add(HEADER_SIZE)
    }

    /// The size in bytes, header included, of this log up to `next_id`.
    pub fn size(&self, next_id: u64) -> Option<usize> {
        Self::size_between(self.start_id(), next_id)
    }

    /// The size in bytes of the body of this log up to `next_id`.
    pub fn body_size(&self, next_id: u64) -> Option<usize> {
        self.size(next_id).map(|s| s - HEADER_SIZE)
    }

    /// The position of `id` in the buffer.  `id` may be the end id.
    fn offset(&self, id: u64) -> Result<usize> {
        let offset = self.size(id).ok_or(Error::IdOutOfRange)?;
        if offset > self.as_bytes().len() {
            return Err(Error::IdOutOfRange);
        }
        Ok(offset)
    }

    fn entry_bytes(&self, id: u64) -> Result<&[u8]> {
        let bytes = &self.as_bytes()[self.offset(id)?..];
        if bytes.is_empty() {
            return Err(Error::IdOutOfRange);
        }
        Ok(bytes)
    }

    pub fn entry(&self, id: u64) -> Result<Entry<'_>> {
        Entry::parse(self.entry_bytes(id)?)
    }

    pub fn is_tuple(&self, id: u64) -> Result<bool> {
        Ok(self.entry_bytes(id)?[0] >> 7 == TUPLE_TAG)
    }

    pub fn is_value(&self, id: u64) -> Result<bool> {
        self.is_tuple(id).map(|t| !t)
    }

    pub fn value(&self, id: u64) -> Result<Value<'_>> {
        match self.entry(id)? {
            Entry::Value(v) => Ok(v),
            Entry::Tuple(_) => Err(Error::KindMismatch),
        }
    }

    /// The data of the value at `id`.
    pub fn blob(&self, id: u64) -> Result<&[u8]> {
        self.value(id).map(|v| v.as_bytes())
    }

    pub fn value_len(&self, id: u64) -> Result<usize> {
        self.value(id).map(|v| v.len())
    }

    pub fn tuple(&self, id: u64) -> Result<Tuple<'_>> {
        match self.entry(id)? {
            Entry::Tuple(t) => Ok(t),
            Entry::Value(_) => Err(Error::KindMismatch),
        }
    }

    pub fn tuple_len(&self, id: u64) -> Result<usize> {
        self.tuple(id).map(|t| t.len())
    }

    /// Appends the ids of the tuple at `id`, decoded relative to `base`, to
    /// `out`.  Returns the extra bit and the id of the following entry.
    pub fn decode_tuple_with_offset(
        &self,
        id: u64,
        base: u64,
        out: &mut Vec<u64>,
    ) -> Result<(bool, u64)> {
        let t = self.tuple(id)?;
        Ok((t.decode_into(base, out), id + t.encoded_len() as u64))
    }

    /// Appends the ids of the tuple at `id` to `out`.  Returns the extra bit
    /// and the id of the following entry.
    pub fn decode_tuple(&self, id: u64, out: &mut Vec<u64>) -> Result<(bool, u64)> {
        self.decode_tuple_with_offset(id, id, out)
    }
}

impl<B: AsRef<[u8]> + AsMut<[u8]>> RecordLog<B> {
    /// Writes a default header for a log starting at `start_id` into `buf`.
    pub fn make(mut buf: B, start_id: u64) -> Result<Self> {
        PersistentHeader::new(start_id).write(buf.as_mut())?;
        Ok(RecordLog { buf })
    }

    pub fn set_header(&mut self, header: &PersistentHeader) {
        header
            .write(self.buf.as_mut())
            .expect("adopted log has a header");
    }

    fn entry_bytes_mut(&mut self, id: u64) -> Result<&mut [u8]> {
        let offset = self.offset(id)?;
        Ok(&mut self.buf.as_mut()[offset..])
    }

    /// Writes a node (an empty value) at `id`, returning the next id.
    pub fn make_node(&mut self, id: u64) -> Result<u64> {
        self.make_value(id, &[])
    }

    /// Writes a value holding `data` at `id`, returning the next id.
    pub fn make_value(&mut self, id: u64, data: &[u8]) -> Result<u64> {
        let n = encode_value(self.entry_bytes_mut(id)?, data)?;
        Ok(id + n as u64)
    }

    /// Writes a tuple of `ids`, encoded relative to `base`, at `id`, returning
    /// the next id.
    pub fn make_tuple_with_offset(
        &mut self,
        id: u64,
        base: u64,
        extra: bool,
        ids: &[u64],
    ) -> Result<u64> {
        let n = encode_tuple(self.entry_bytes_mut(id)?, base, extra, ids)?;
        Ok(id + n as u64)
    }

    /// Writes a tuple of `ids` at `id`, returning the next id.
    pub fn make_tuple(&mut self, id: u64, extra: bool, ids: &[u64]) -> Result<u64> {
        self.make_tuple_with_offset(id, id, extra, ids)
    }
}

impl RecordLog<Vec<u8>> {
    /// An empty, growable log whose first entry will have `start_id`.
    pub fn new(start_id: u64) -> Self {
        RecordLog {
            buf: PersistentHeader::new(start_id).to_bytes().to_vec(),
        }
    }

    /// Appends an entry of at most `max_len` bytes written by `encode`,
    /// returning its id.
    fn push(
        &mut self,
        max_len: usize,
        encode: impl FnOnce(&mut [u8], u64) -> Result<usize>,
    ) -> Result<u64> {
        let id = self.end_id();
        let start = self.buf.len();
        self.buf.resize(start + max_len, 0);
        match encode(&mut self.buf[start..], id) {
            Ok(n) => {
                self.buf.truncate(start + n);
                Ok(id)
            }
            Err(e) => {
                self.buf.truncate(start);
                Err(e)
            }
        }
    }

    /// Appends a node, returning its id.
    pub fn push_node(&mut self) -> u64 {
        self.push_value(&[])
    }

    /// Appends a value holding `data`, returning its id.
    pub fn push_value(&mut self, data: &[u8]) -> u64 {
        self.push(max_value_len(data.len()), |out, _| encode_value(out, data))
            .expect("buffer sized for value")
    }

    /// Appends a tuple of `ids`, returning its id.  Fails with
    /// [`Error::Overflow`] if an id exceeds `i64::MAX`.
    pub fn push_tuple(&mut self, extra: bool, ids: &[u64]) -> Result<u64> {
        self.push(max_tuple_len(ids.len()), |out, id| {
            encode_tuple(out, id, extra, ids)
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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

    #[test]
    fn header_layout() {
        let h = PersistentHeader {
            magic: MAGIC,
            version: 2,
            compression: 3,
            written: 4,
            reserved: 5,
            start_id: 0x0102_0304_0506_0708,
        };
        let b = h.to_bytes();
        assert_eq!(&b[0..4], &MAGIC.to_le_bytes());
        assert_eq!(&b[4..8], &[2, 3, 4, 5]);
        assert_eq!(&b[8..16], &[8, 7, 6, 5, 4, 3, 2, 1]);
        assert_eq!(PersistentHeader::read(&b).unwrap(), h);
    }

    #[test]
    fn new_log_is_valid() {
        let log = RecordLog::new(1);
        assert!(log.is_valid());
        assert_eq!(log.start_id(), 1);
        assert_eq!(log.end_id(), 1);
        assert_eq!(log.as_bytes().len(), HEADER_SIZE);
        let mut bad = log.into_inner();
        bad[0] ^= 1;
        assert!(!RecordLog::adopt(bad).unwrap().is_valid());
        assert_eq!(RecordLog::adopt(vec![0; 3]), Err(Error::Truncated));
    }

    #[test]
    fn nodes_and_values() {
        let mut log = RecordLog::new(1);
        let node = log.push_node();
        let hello = log.push_value(b"hello");
        let big = vec![7u8; 1000];
        let big_id = log.push_value(&big);
        assert_eq!(node, 1);
        assert_eq!(hello, 2);
        assert_eq!(big_id, hello + 6);
        assert_eq!(
            log.as_bytes()[HEADER_SIZE],
            0,
            "a node is a single zero byte"
        );

        assert!(log.value(node).unwrap().is_empty());
        assert_eq!(log.blob(hello).unwrap(), b"hello");
        assert_eq!(log.value_len(big_id).unwrap(), 1000);
        assert_eq!(log.blob(big_id).unwrap(), &big[..]);
        assert!(log.is_value(hello).unwrap());
        assert_eq!(log.tuple(hello), Err(Error::KindMismatch));
        assert_eq!(log.entry(log.end_id()), Err(Error::IdOutOfRange));
        assert_eq!(log.entry(0), Err(Error::IdOutOfRange));
    }

    #[test]
    fn tuples_round_trip() {
        let mut rng = XorShift(0x9e37_79b9_7f4a_7c15);
        let mut log = RecordLog::new(1);
        let mut ids = vec![log.push_node()];
        let mut tuples = Vec::new();
        for n in 0..=17 {
            let tuple_ids: Vec<u64> = (0..n)
                .map(|i| match i % 3 {
                    0 => ids[(rng.next() as usize) % ids.len()],
                    1 => rng.next() >> 1,
                    _ => rng.next() % 300,
                })
                .collect();
            let extra = n % 2 == 1;
            let id = log.push_tuple(extra, &tuple_ids).unwrap();
            assert!(log.as_bytes().len() - log.offset(id).unwrap() <= max_tuple_len(n));
            ids.push(id);
            tuples.push((id, extra, tuple_ids));
        }
        for (id, extra, expected) in &tuples {
            let t = log.tuple(*id).unwrap();
            assert!(log.is_tuple(*id).unwrap());
            assert_eq!(t.len(), expected.len());
            assert_eq!(t.extra_bit(), *extra && !expected.is_empty());
            for (i, &e) in expected.iter().enumerate() {
                assert_eq!(t.id(i, *id), Some(e));
            }
            assert_eq!(t.field(expected.len()), None);
            let mut out = Vec::new();
            let (got_extra, next) = log.decode_tuple(*id, &mut out).unwrap();
            assert_eq!(&out, expected);
            assert_eq!(got_extra, t.extra_bit());
            assert_eq!(next, id + t.encoded_len() as u64);
        }
    }

    #[test]
    fn walk_log() {
        let mut log = RecordLog::new(10);
        let a = log.push_node();
        let b = log.push_value(b"b");
        let t = log.push_tuple(true, &[a, b]).unwrap();
        let mut id = log.start_id();
        let mut seen = Vec::new();
        while id < log.end_id() {
            let e = log.entry(id).unwrap();
            seen.push((id, e.is_tuple()));
            id += e.encoded_len() as u64;
        }
        assert_eq!(seen, vec![(a, false), (b, false), (t, true)]);
    }

    #[test]
    fn nearby_references_are_relative() {
        let mut log = RecordLog::new(1 << 40);
        let a = log.push_node();
        let t = log.push_tuple(false, &[a]).unwrap();
        let tuple = log.tuple(t).unwrap();
        assert_eq!(tuple.field(0), Some(1), "one entry back");
        assert_eq!(tuple.encoded_len(), 3, "count + prefix + 1 byte field");
        assert_eq!(tuple.decode(t), (vec![a], false));
        // Decoding against a different base changes relative ids.
        let mut out = Vec::new();
        log.decode_tuple_with_offset(t, t + 5, &mut out).unwrap();
        assert_eq!(out, vec![a + 5]);
    }

    #[test]
    fn oversized_id_is_rejected() {
        let mut log = RecordLog::new(1);
        let before = log.as_bytes().len();
        assert_eq!(log.push_tuple(false, &[u64::MAX]), Err(Error::Overflow));
        assert_eq!(log.as_bytes().len(), before);
    }

    #[test]
    fn complete_len_detects_truncation() {
        let mut log = RecordLog::new(1);
        let v = log.push_value(b"some value");
        let t = log.push_tuple(false, &[1, 2, 3, 4, 5, 6, 7]).unwrap();
        for id in [v, t] {
            let bytes = &log.as_bytes()[log.offset(id).unwrap()..];
            let len = log.entry(id).unwrap().encoded_len();
            assert_eq!(Entry::complete_len(&bytes[..len]), Some(len));
            for cut in 0..len {
                assert_eq!(Entry::complete_len(&bytes[..cut]), None);
            }
        }
    }

    #[test]
    fn estimate_len_is_conservative() {
        let mut log = RecordLog::new(1);
        let ids: Vec<u64> = (0..23).map(|i| i * 1_000_003).collect();
        let t = log.push_tuple(false, &ids).unwrap();
        let bytes = &log.as_bytes()[log.offset(t).unwrap()..];
        let len = log.tuple(t).unwrap().encoded_len();
        assert_eq!(Tuple::estimate_len(bytes), Some(len));
        for cut in 1..len {
            assert!(Tuple::estimate_len(&bytes[..cut]).unwrap() >= len);
        }
        assert_eq!(Tuple::estimate_len(&[0]), None, "a value is not a tuple");
    }

    #[test]
    fn write_in_place() {
        let mut buf = [0u8; 64];
        let mut log = RecordLog::make(&mut buf[..], 1).unwrap();
        let mut next = log.make_node(1).unwrap();
        let v = next;
        next = log.make_value(v, b"xyz").unwrap();
        let t = next;
        next = log.make_tuple(t, true, &[1, v]).unwrap();
        assert_eq!(log.make_value(next, &[0; 64]), Err(Error::BufferTooSmall));
        assert_eq!(log.blob(v).unwrap(), b"xyz");
        assert_eq!(log.tuple(t).unwrap().decode(t), (vec![1, v], true));

        let size = log.size(next).unwrap();
        let persisted = log.bytes_until(next).unwrap().to_vec();
        assert_eq!(persisted.len(), size);
        let reopened = RecordLog::adopt(persisted).unwrap();
        assert!(reopened.is_valid());
        assert_eq!(reopened.end_id(), next);
        assert_eq!(reopened.next_id(size), next);
        assert_eq!(reopened.body_size(next), Some(size - HEADER_SIZE));
    }

    /// Writes tuples of `lens` ids taken in order from `ids` in place into a
    /// log starting at `start_id` until `n` ids are written, then decodes them
    /// all back.  `ids` must hold at least `n + lens.end() - 1` ids.
    fn round_trip_in_place(
        start_id: u64,
        n: usize,
        ids: &[u64],
        lens: std::ops::RangeInclusive<usize>,
        rng: &mut XorShift,
    ) {
        // Every tuple holds at least one id, so `max_tuple_len(1)` per id is
        // enough room.
        let mut buf = vec![0u8; HEADER_SIZE + max_tuple_len(1) * ids.len()];
        let mut log = RecordLog::make(&mut buf[..], start_id).unwrap();
        let span = (lens.end() - lens.start() + 1) as u64;
        let mut tuples = Vec::new();
        let mut id = start_id;
        let mut last = 0;
        while last < n {
            let len = lens.start() + (rng.next() % span) as usize;
            let extra = rng.next() & 1 == 1;
            tuples.push((id, last, len, extra));
            id = log.make_tuple(id, extra, &ids[last..last + len]).unwrap();
            last += len;
        }
        let end = id;

        let mut id = start_id;
        let mut out = Vec::new();
        for &(tuple_id, first, len, extra) in &tuples {
            assert_eq!(id, tuple_id);
            assert_eq!(log.tuple_len(id).unwrap(), len);
            out.clear();
            let (got_extra, next) = log.decode_tuple(id, &mut out).unwrap();
            assert_eq!(got_extra, extra);
            assert_eq!(out, &ids[first..first + len]);
            id = next;
        }
        assert_eq!(id, end);
    }

    /// Ported from the C++ RecordLogRandom test: skewed ids, so small and
    /// large values are both common, in tuples of 1 to 15 ids.
    #[test]
    fn random_skewed_round_trip() {
        let n = 100_000;
        let mut rng = XorShift(0x2545_f491_4f6c_dd1d);
        // Ids are limited to 63 bits, the most `encode_field` accepts.
        let ids: Vec<u64> = (0..n + 15)
            .map(|_| {
                let bits = rng.next() % 64;
                rng.next() & ((1u64 << bits) - 1)
            })
            .collect();
        round_trip_in_place(1, n, &ids, 1..=15, &mut rng);
    }

    /// Ported from the C++ RecordLogCompressDecompressVersion1 test, without
    /// the compression step: ids uniform below `start_id`, so every id is a
    /// back reference, in tuples of 6 to 15 ids.
    #[test]
    fn random_back_reference_round_trip() {
        let n = 100_000;
        let mut rng = XorShift(0x9e37_79b9_7f4a_7c15);
        let ids: Vec<u64> = (0..n + 15)
            .map(|_| 1 + rng.next() % (n as u64 - 1))
            .collect();
        round_trip_in_place(n as u64, n, &ids, 6..=15, &mut rng);
    }
}
