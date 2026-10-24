//! Record log encode and decode benchmarks, ported from the C++
//! BM_RecordLogEncode and BM_RecordLogDecode benchmarks.

use std::hint::black_box;

use criterion::{Criterion, Throughput, criterion_group, criterion_main};
use gdblite::RecordLog;
use gdblite::record_log::{HEADER_SIZE, max_tuple_len};

/// The number of ids written per iteration.
const IDS: usize = 100_000;

/// A small deterministic generator.
struct XorShift(u64);

impl XorShift {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
}

/// Skewed ids (random bit width up to 63) and tuple lengths of 1 to 15 ids
/// that together cover `IDS` ids.
fn inputs() -> (Vec<u64>, Vec<usize>) {
    let mut rng = XorShift(0x2545_f491_4f6c_dd1d);
    let ids: Vec<u64> = (0..IDS + 15)
        .map(|_| {
            let bits = rng.next() % 64;
            rng.next() & ((1u64 << bits) - 1)
        })
        .collect();
    let mut lens = Vec::new();
    let mut total = 0;
    while total < IDS {
        let len = 1 + (rng.next() % 15) as usize;
        lens.push(len);
        total += len;
    }
    (ids, lens)
}

/// Writes tuples of `lens` ids from `ids` in place, returning the next id.
fn encode(log: &mut RecordLog<&mut [u8]>, ids: &[u64], lens: &[usize]) -> u64 {
    let mut id = log.start_id();
    let mut first = 0;
    for &len in lens {
        id = log.make_tuple(id, false, &ids[first..first + len]).unwrap();
        first += len;
    }
    id
}

fn bench_record_log(c: &mut Criterion) {
    let (ids, lens) = inputs();
    let written: usize = lens.iter().sum();
    let mut buf = vec![0u8; HEADER_SIZE + max_tuple_len(1) * ids.len()];

    let mut group = c.benchmark_group("record_log");
    group.throughput(Throughput::Elements(written as u64));

    group.bench_function("encode", |b| {
        let mut log = RecordLog::make(&mut buf[..], 1).unwrap();
        b.iter(|| encode(&mut log, black_box(&ids), black_box(&lens)))
    });

    let mut log = RecordLog::make(&mut buf[..], 1).unwrap();
    let end = encode(&mut log, &ids, &lens);
    let mut out = Vec::with_capacity(written);
    group.bench_function("decode", |b| {
        b.iter(|| {
            out.clear();
            let mut id = log.start_id();
            while id < end {
                id = log.decode_tuple(black_box(id), &mut out).unwrap().1;
            }
            black_box(&out);
        })
    });

    group.finish();
}

criterion_group!(benches, bench_record_log);
criterion_main!(benches);
