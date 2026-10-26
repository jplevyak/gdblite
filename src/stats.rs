//! Statistics of a graph's links, for estimating the cost of queries.
//!
//! For each predicate, [`Stats`] counts the links with it, live and dead, and
//! estimates how many distinct subjects and objects they have.  The counts
//! are exact for the entries appended; the distinct counts are exact up to
//! [`EXACT_DISTINCT`] and HyperLogLog estimates beyond, within about 7%.
//! Neither decreases when links are deleted, so the distinct counts are upper
//! bounds on those of live links.
//!
//! A [`Graph`](crate::graph::Graph) keeps its statistics in memory and saves
//! them to `{prefix}stats` from time to time; on open it loads them and
//! counts the entries committed since they were saved.

use std::collections::HashMap;
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};

use crate::mmap::{suffixed, sync_dir};
use crate::record::{Record, Uuid};

/// The most distinct items a [`Distinct`] counts exactly.
pub const EXACT_DISTINCT: usize = 32;

const REGISTER_BITS: u32 = 8;
const REGISTERS: usize = 1 << REGISTER_BITS;

const FILE_MAGIC: u64 = u64::from_le_bytes(*b"gdblstat");
const FILE_VERSION: u64 = 1;

/// Mixes the bits of `x` (the splitmix64 finalizer).
fn mix(mut x: u64) -> u64 {
    x = (x ^ (x >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    x = (x ^ (x >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    x ^ (x >> 31)
}

/// An estimate of the number of distinct ids added.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Distinct {
    /// The sorted hashes of up to [`EXACT_DISTINCT`] ids.
    Exact(Vec<u64>),
    /// HyperLogLog registers.
    Sketch(Box<[u8; REGISTERS]>),
}

impl Default for Distinct {
    fn default() -> Self {
        Distinct::Exact(Vec::new())
    }
}

impl Distinct {
    pub fn insert(&mut self, id: u64) {
        let h = mix(id);
        match self {
            Distinct::Exact(hashes) => {
                let Err(i) = hashes.binary_search(&h) else {
                    return;
                };
                if hashes.len() < EXACT_DISTINCT {
                    hashes.insert(i, h);
                    return;
                }
                let mut registers = Box::new([0u8; REGISTERS]);
                for &h in hashes.iter().chain([&h]) {
                    Self::add(&mut registers, h);
                }
                *self = Distinct::Sketch(registers);
            }
            Distinct::Sketch(registers) => Self::add(registers, h),
        }
    }

    fn add(registers: &mut [u8; REGISTERS], h: u64) {
        let i = (h >> (64 - REGISTER_BITS)) as usize;
        let rank = ((h << REGISTER_BITS).leading_zeros() + 1).min(64 - REGISTER_BITS + 1) as u8;
        registers[i] = registers[i].max(rank);
    }

    /// The estimated number of distinct ids.
    pub fn len(&self) -> u64 {
        match self {
            Distinct::Exact(hashes) => hashes.len() as u64,
            Distinct::Sketch(registers) => {
                let m = REGISTERS as f64;
                let alpha = 0.7213 / (1.0 + 1.079 / m);
                let sum: f64 = registers.iter().map(|&r| (-f64::from(r)).exp2()).sum();
                let estimate = alpha * m * m / sum;
                let zeros = registers.iter().filter(|&&r| r == 0).count();
                let estimate = if estimate <= 2.5 * m && zeros > 0 {
                    // Linear counting is better for small counts.
                    m * (m / zeros as f64).ln()
                } else {
                    estimate
                };
                (estimate.round() as u64).max(EXACT_DISTINCT as u64 + 1)
            }
        }
    }

    pub fn is_empty(&self) -> bool {
        matches!(self, Distinct::Exact(hashes) if hashes.is_empty())
    }

    fn write(&self, out: &mut impl Write) -> io::Result<()> {
        match self {
            Distinct::Exact(hashes) => {
                out.write_all(&[0, hashes.len() as u8])?;
                for h in hashes {
                    out.write_all(&h.to_le_bytes())?;
                }
            }
            Distinct::Sketch(registers) => {
                out.write_all(&[1])?;
                out.write_all(&registers[..])?;
            }
        }
        Ok(())
    }

    fn read(input: &mut impl Read) -> io::Result<Self> {
        let mut tag = [0u8];
        input.read_exact(&mut tag)?;
        match tag[0] {
            0 => {
                let mut len = [0u8];
                input.read_exact(&mut len)?;
                let mut hashes = Vec::with_capacity(len[0] as usize);
                for _ in 0..len[0] {
                    hashes.push(read_word(input)?);
                }
                if len[0] as usize > EXACT_DISTINCT || !hashes.is_sorted() {
                    return Err(invalid());
                }
                Ok(Distinct::Exact(hashes))
            }
            1 => {
                let mut registers = Box::new([0u8; REGISTERS]);
                input.read_exact(&mut registers[..])?;
                Ok(Distinct::Sketch(registers))
            }
            _ => Err(invalid()),
        }
    }
}

/// The statistics of the links with one predicate.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PredicateStats {
    /// The links appended, live and dead.
    pub links: u64,
    /// The dead links appended.
    pub dead: u64,
    pub subjects: Distinct,
    pub objects: Distinct,
}

impl PredicateStats {
    /// The live links: each dead link appended deletes a live one.
    pub fn live(&self) -> u64 {
        (self.links - self.dead).saturating_sub(self.dead)
    }

    /// The estimated live links per distinct subject.
    pub fn per_subject(&self) -> f64 {
        self.live() as f64 / self.subjects.len().max(1) as f64
    }

    /// The estimated live links per distinct object.
    pub fn per_object(&self) -> f64 {
        self.live() as f64 / self.objects.len().max(1) as f64
    }
}

/// The statistics of a graph's links; see the [module](self) docs.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Stats {
    predicates: HashMap<u64, PredicateStats>,
    links: u64,
    dead: u64,
}

impl Stats {
    /// Counts `record`.
    pub fn add(&mut self, record: &Record<'_>) {
        let Some(link) = record.as_link() else {
            return;
        };
        let p = link.predicate();
        if p == 0 {
            return;
        }
        let stats = self.predicates.entry(p).or_default();
        stats.links += 1;
        self.links += 1;
        if link.dead() {
            stats.dead += 1;
            self.dead += 1;
        }
        stats.subjects.insert(link.subject());
        stats.objects.insert(link.object());
    }

    /// The statistics of the links with `predicate`, if there are any.
    pub fn predicate(&self, predicate: u64) -> Option<&PredicateStats> {
        self.predicates.get(&predicate)
    }

    /// The statistics of each predicate.
    pub fn predicates(&self) -> impl Iterator<Item = (u64, &PredicateStats)> {
        self.predicates.iter().map(|(&p, s)| (p, s))
    }

    /// The number of distinct predicates.
    pub fn predicate_count(&self) -> usize {
        self.predicates.len()
    }

    /// The links appended, live and dead.
    pub fn links(&self) -> u64 {
        self.links
    }

    /// The dead links appended.
    pub fn dead(&self) -> u64 {
        self.dead
    }

    /// The live links.
    pub fn live(&self) -> u64 {
        (self.links - self.dead).saturating_sub(self.dead)
    }
}

fn read_word(input: &mut impl Read) -> io::Result<u64> {
    let mut b = [0u8; 8];
    input.read_exact(&mut b)?;
    Ok(u64::from_le_bytes(b))
}

fn invalid() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, "invalid statistics file")
}

/// The statistics file of the graph at `prefix`.
pub(crate) fn path(prefix: &Path) -> PathBuf {
    suffixed(prefix, "stats")
}

/// Saves `stats`, which cover the entries before `next_id` of the log with
/// `uuid`, to the statistics file at `prefix`, replacing it atomically.
pub(crate) fn save(prefix: &Path, uuid: Uuid, next_id: u64, stats: &Stats) -> io::Result<()> {
    let tmp = suffixed(prefix, "stats.tmp");
    let mut out = io::BufWriter::new(File::create(&tmp)?);
    for word in [
        FILE_MAGIC,
        FILE_VERSION,
        uuid.uuid0,
        uuid.uuid1,
        next_id,
        stats.predicates.len() as u64,
    ] {
        out.write_all(&word.to_le_bytes())?;
    }
    for (&p, s) in &stats.predicates {
        for word in [p, s.links, s.dead] {
            out.write_all(&word.to_le_bytes())?;
        }
        s.subjects.write(&mut out)?;
        s.objects.write(&mut out)?;
    }
    out.into_inner()?.sync_all()?;
    fs::rename(&tmp, path(prefix))?;
    sync_dir(prefix)
}

/// Loads the statistics saved at `prefix` for the log with `uuid`, and the
/// id they cover entries before, or `None` if there are none or they are not
/// valid.
pub(crate) fn load(prefix: &Path, uuid: Uuid) -> Option<(Stats, u64)> {
    let mut input = io::BufReader::new(File::open(path(prefix)).ok()?);
    let result = (|| -> io::Result<Option<(Stats, u64)>> {
        let input = &mut input;
        let header: Vec<u64> = (0..6)
            .map(|_| read_word(input))
            .collect::<io::Result<_>>()?;
        let [magic, version, uuid0, uuid1, next_id, count] = header[..] else {
            unreachable!()
        };
        if (magic, version) != (FILE_MAGIC, FILE_VERSION)
            || (uuid0, uuid1) != (uuid.uuid0, uuid.uuid1)
        {
            return Ok(None);
        }
        let mut stats = Stats::default();
        for _ in 0..count {
            let (p, links, dead) = (read_word(input)?, read_word(input)?, read_word(input)?);
            let s = PredicateStats {
                links,
                dead,
                subjects: Distinct::read(input)?,
                objects: Distinct::read(input)?,
            };
            if p == 0 || dead > links || stats.predicates.insert(p, s).is_some() {
                return Ok(None);
            }
            stats.links += links;
            stats.dead += dead;
        }
        if input.read(&mut [0])? != 0 {
            return Ok(None);
        }
        Ok(Some((stats, next_id)))
    })();
    result.ok().flatten()
}

/// Removes the statistics file at `prefix`, if there is one.
pub(crate) fn remove(prefix: &Path) -> io::Result<()> {
    for p in [path(prefix), suffixed(prefix, "stats.tmp")] {
        match fs::remove_file(p) {
            Err(e) if e.kind() != io::ErrorKind::NotFound => return Err(e),
            _ => {}
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn distinct() {
        let mut d = Distinct::default();
        assert!(d.is_empty());
        for i in 1..=EXACT_DISTINCT as u64 {
            d.insert(i);
            d.insert(i);
        }
        assert_eq!(d.len(), EXACT_DISTINCT as u64);
        assert!(matches!(d, Distinct::Exact(_)));
        for (n, expect_error) in [(100u64, 0.15), (1_000, 0.15), (100_000, 0.2)] {
            let mut d = Distinct::default();
            for i in 0..n {
                d.insert(i * 7 + 1);
                d.insert(i * 7 + 1);
            }
            let error = (d.len() as f64 - n as f64).abs() / n as f64;
            assert!(error < expect_error, "{n}: {} ({error})", d.len());
        }
    }

    #[test]
    fn distinct_io() {
        for n in [0, 5, 1000] {
            let mut d = Distinct::default();
            (1..=n).for_each(|i| d.insert(i));
            let mut buf = Vec::new();
            d.write(&mut buf).unwrap();
            assert_eq!(Distinct::read(&mut &buf[..]).unwrap(), d);
        }
        assert!(Distinct::read(&mut &[7u8][..]).is_err());
    }
}
