//! Variable-length integer encodings used by the record log.
//!
//! # Tagged variable integer
//!
//! An unsigned integer prefixed by `tag_bits` high-order tag bits.  The first
//! byte is laid out (most significant bit first) as
//!
//! ```text
//! [ tag : tag_bits ][ more : 1 ][ value : 7 - tag_bits ]
//! ```
//!
//! and each following byte as `[ more : 1 ][ value : 7 ]`, least significant
//! group first.  With one tag bit, 7 bytes hold a 48 bit value.
//!
//! # Group variable integer
//!
//! One to five unsigned integers stored as a length prefix followed by each
//! integer in little endian using its minimal byte length (1 to 8 bytes).  Each
//! length is stored as `len - 1` in 3 bits.  Groups of one or two integers use
//! a one byte prefix; groups of three to five use a two byte little endian
//! prefix.  In both forms bit 7 of the first prefix byte is the *extra bit*, a
//! free flag carried alongside the integers, and the length of integer `i`
//! occupies the 3 bits starting at bit `3 * i` of the remaining 15 (or 7) bits.
//!
//! # Fields
//!
//! Ids stored in a group are encoded relative to a base id with
//! [`encode_field`]: an id below the base may be stored as a negative offset
//! from it, otherwise it is stored as an absolute value, both zigzag encoded.

use crate::{Error, Result};

/// The largest number of integers in one group.
pub const GROUP_MAX: usize = 5;

/// Bit 7 of the first prefix byte of a group holds the extra bit.
const EXTRA_BIT: u8 = 0x80;

fn low_mask(bits: u32) -> u64 {
    (1u64 << bits) - 1
}

/// The number of bytes [`encode_tagged_varint`] uses for `value`.
pub fn tagged_varint_len(tag_bits: u32, value: u64) -> usize {
    debug_assert!((1..=6).contains(&tag_bits));
    let mut rest = value >> (7 - tag_bits);
    let mut len = 1;
    while rest != 0 {
        rest >>= 7;
        len += 1;
    }
    len
}

/// Encodes `value` with the low `tag_bits` bits of `tag` into the front of
/// `out`, returning the number of bytes written.
pub fn encode_tagged_varint(out: &mut [u8], tag: u8, tag_bits: u32, value: u64) -> Result<usize> {
    let len = tagged_varint_len(tag_bits, value);
    let out = out.get_mut(..len).ok_or(Error::BufferTooSmall)?;
    let first_bits = 7 - tag_bits;
    let more = if len > 1 { 1u8 << first_bits } else { 0 };
    out[0] = (tag << (8 - tag_bits)) | more | (value & low_mask(first_bits)) as u8;
    let mut rest = value >> first_bits;
    for (i, b) in out.iter_mut().enumerate().skip(1) {
        let more = if i + 1 < len { 0x80 } else { 0 };
        *b = more | (rest & 0x7f) as u8;
        rest >>= 7;
    }
    Ok(len)
}

/// Decodes a tagged variable integer from the front of `input`, returning
/// `(tag, value, bytes consumed)`.
pub fn decode_tagged_varint(input: &[u8], tag_bits: u32) -> Result<(u8, u64, usize)> {
    debug_assert!((1..=6).contains(&tag_bits));
    let first = *input.first().ok_or(Error::Truncated)?;
    let first_bits = 7 - tag_bits;
    let tag = first >> (8 - tag_bits);
    let mut value = u64::from(first) & low_mask(first_bits);
    let mut more = first & (1 << first_bits) != 0;
    let mut shift = first_bits;
    let mut len = 1;
    while more {
        let b = *input.get(len).ok_or(Error::Truncated)?;
        let bits = u64::from(b & 0x7f);
        if shift >= 64 || (shift > 57 && bits >> (64 - shift) != 0) {
            return Err(Error::Overflow);
        }
        value |= bits << shift;
        shift += 7;
        more = b & 0x80 != 0;
        len += 1;
    }
    Ok((tag, value, len))
}

/// The number of prefix bytes in a group of `count` integers.
pub fn group_prefix_len(count: usize) -> usize {
    debug_assert!((1..=GROUP_MAX).contains(&count));
    if count <= 2 { 1 } else { 2 }
}

/// The minimal number of bytes (1 to 8) holding `value`.
fn byte_len(value: u64) -> usize {
    ((64 - value.leading_zeros() as usize).div_ceil(8)).max(1)
}

/// Reads a group prefix, returning `(extra bit, packed lengths, prefix bytes)`.
fn read_prefix(input: &[u8], count: usize) -> Result<(bool, u16, usize)> {
    let prefix_len = group_prefix_len(count);
    let prefix = input.get(..prefix_len).ok_or(Error::Truncated)?;
    let raw = if prefix_len == 1 {
        u16::from(prefix[0])
    } else {
        u16::from_le_bytes([prefix[0], prefix[1]])
    };
    // Remove the extra bit (bit 7), closing the gap it leaves.
    let packed = (raw & 0x7f) | ((raw >> 1) & 0x7f80);
    Ok((prefix[0] & EXTRA_BIT != 0, packed, prefix_len))
}

fn field_len(packed: u16, i: usize) -> usize {
    usize::from((packed >> (3 * i)) & 7) + 1
}

/// Encodes 1 to 5 `values` and the `extra` bit into the front of `out`,
/// returning the number of bytes written.
pub fn encode_group(out: &mut [u8], extra: bool, values: &[u64]) -> Result<usize> {
    let count = values.len();
    assert!((1..=GROUP_MAX).contains(&count), "group of {count} values");
    let prefix_len = group_prefix_len(count);
    let total = prefix_len + values.iter().map(|&v| byte_len(v)).sum::<usize>();
    let out = out.get_mut(..total).ok_or(Error::BufferTooSmall)?;
    let packed = values
        .iter()
        .enumerate()
        .fold(0u16, |p, (i, &v)| p | ((byte_len(v) - 1) as u16) << (3 * i));
    // Open a gap at bit 7 for the extra bit.
    let raw = (packed & 0x7f) | ((packed & 0x7f80) << 1) | if extra { 0x80 } else { 0 };
    out[..prefix_len].copy_from_slice(&raw.to_le_bytes()[..prefix_len]);
    let mut pos = prefix_len;
    for &v in values {
        let n = byte_len(v);
        out[pos..pos + n].copy_from_slice(&v.to_le_bytes()[..n]);
        pos += n;
    }
    Ok(total)
}

/// The encoded length of the group of `count` integers at the front of
/// `input`.  Only the prefix is read, so the result may exceed `input.len()`.
pub fn group_len(input: &[u8], count: usize) -> Result<usize> {
    let (_, packed, prefix_len) = read_prefix(input, count)?;
    Ok(prefix_len + (0..count).map(|i| field_len(packed, i)).sum::<usize>())
}

/// Decodes the group of `out.len()` integers at the front of `input` into
/// `out`, returning `(extra bit, bytes consumed)`.
pub fn decode_group(input: &[u8], out: &mut [u64]) -> Result<(bool, usize)> {
    let (extra, packed, mut pos) = read_prefix(input, out.len())?;
    for (i, v) in out.iter_mut().enumerate() {
        let n = field_len(packed, i);
        let bytes = input.get(pos..pos + n).ok_or(Error::Truncated)?;
        let mut le = [0u8; 8];
        le[..n].copy_from_slice(bytes);
        *v = u64::from_le_bytes(le);
        pos += n;
    }
    Ok((extra, pos))
}

/// Decodes integer `i` of the group of `count` integers at the front of
/// `input`.
pub fn group_field(input: &[u8], count: usize, i: usize) -> Result<u64> {
    debug_assert!(i < count);
    let (_, packed, prefix_len) = read_prefix(input, count)?;
    let pos = prefix_len + (0..i).map(|j| field_len(packed, j)).sum::<usize>();
    let n = field_len(packed, i);
    let bytes = input.get(pos..pos + n).ok_or(Error::Truncated)?;
    let mut le = [0u8; 8];
    le[..n].copy_from_slice(bytes);
    Ok(u64::from_le_bytes(le))
}

/// Encodes `id` relative to `base`, choosing the smaller of a negative offset
/// from `base` (when `id < base`) and the absolute id.  Returns `None` when
/// `id` exceeds `i64::MAX`.
pub fn encode_field(base: u64, id: u64) -> Option<u64> {
    let absolute = id.checked_mul(2)?;
    let relative = (base > id)
        .then(|| (base - id).checked_mul(2).map(|r| r - 1))
        .flatten();
    Some(relative.map_or(absolute, |r| r.min(absolute)))
}

/// Inverts [`encode_field`].
pub fn decode_field(base: u64, value: u64) -> u64 {
    let offset = ((value >> 1) as i64) ^ -((value & 1) as i64);
    if offset < 0 {
        base.wrapping_add_signed(offset)
    } else {
        offset as u64
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tagged_varint_round_trip() {
        let mut buf = [0u8; 16];
        for &v in &[
            0,
            1,
            63,
            64,
            127,
            128,
            1 << 20,
            (1 << 48) - 1,
            1 << 48,
            u64::MAX,
        ] {
            for tag in 0..2 {
                let n = encode_tagged_varint(&mut buf, tag, 1, v).unwrap();
                assert_eq!(n, tagged_varint_len(1, v));
                assert_eq!(decode_tagged_varint(&buf[..n], 1).unwrap(), (tag, v, n));
                assert_eq!(
                    decode_tagged_varint(&buf[..n - 1], 1),
                    Err(Error::Truncated)
                );
            }
        }
    }

    #[test]
    fn tagged_varint_sizes() {
        assert_eq!(tagged_varint_len(1, 0), 1);
        assert_eq!(tagged_varint_len(1, 63), 1);
        assert_eq!(tagged_varint_len(1, 64), 2);
        assert_eq!(tagged_varint_len(1, (1 << 48) - 1), 7);
        assert_eq!(tagged_varint_len(1, u64::MAX), 10);
    }

    #[test]
    fn tagged_varint_tag_bit_is_high_bit() {
        let mut buf = [0u8; 4];
        encode_tagged_varint(&mut buf, 1, 1, 5).unwrap();
        assert_eq!(buf[0], 0x80 | 5);
        encode_tagged_varint(&mut buf, 0, 1, 0).unwrap();
        assert_eq!(buf[0], 0);
    }

    #[test]
    fn tagged_varint_overflow() {
        let buf = [0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f];
        assert_eq!(decode_tagged_varint(&buf, 1), Err(Error::Overflow));
    }

    #[test]
    fn group_round_trip() {
        let values = [0, 0xff, 0x1_0000, u64::MAX, 42];
        let mut buf = [0u8; 64];
        for count in 1..=GROUP_MAX {
            for extra in [false, true] {
                let n = encode_group(&mut buf, extra, &values[..count]).unwrap();
                assert_eq!(group_len(&buf, count).unwrap(), n);
                assert_eq!(buf[0] & EXTRA_BIT != 0, extra);
                let mut out = [0u64; GROUP_MAX];
                assert_eq!(
                    decode_group(&buf[..n], &mut out[..count]).unwrap(),
                    (extra, n)
                );
                assert_eq!(out[..count], values[..count]);
                for (i, &v) in values[..count].iter().enumerate() {
                    assert_eq!(group_field(&buf, count, i).unwrap(), v);
                }
                assert_eq!(
                    decode_group(&buf[..n - 1], &mut out[..count]),
                    Err(Error::Truncated)
                );
            }
        }
    }

    #[test]
    fn group_too_small() {
        let mut buf = [0u8; 3];
        assert_eq!(
            encode_group(&mut buf, false, &[1 << 16]),
            Err(Error::BufferTooSmall)
        );
    }

    #[test]
    fn field_round_trip() {
        for &(base, id) in &[
            (0, 0),
            (100, 0),
            (100, 99),
            (100, 100),
            (100, 150),
            (1 << 40, 7),
        ] {
            let v = encode_field(base, id).unwrap();
            assert_eq!(decode_field(base, v), id);
        }
        // Nearby earlier ids use a small relative offset.
        assert_eq!(encode_field(1000, 999), Some(1));
        // Small absolute ids beat a large relative offset.
        assert_eq!(encode_field(1000, 3), Some(6));
        assert_eq!(encode_field(0, u64::MAX), None);
    }
}
