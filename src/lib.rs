//! gdblite: a lightweight graph database.
//!
//! The storage layer is an append-only [`RecordLog`] of entries addressed by
//! their byte position (their *id*).  An entry is either a [`Value`] (a byte
//! blob; an empty value is a bare node) or a [`Tuple`] (an ordered list of ids
//! linking earlier entries together).

pub mod coder;
#[cfg(all(unix, target_pointer_width = "64"))]
pub mod dynamic_table;
mod error;
#[cfg(all(unix, target_pointer_width = "64"))]
pub mod local;
#[cfg(all(unix, target_pointer_width = "64"))]
pub mod local_record_log;
#[cfg(all(unix, target_pointer_width = "64"))]
pub mod local_table;
#[cfg(all(unix, target_pointer_width = "64"))]
mod mmap;
pub mod record;
pub mod record_log;
#[cfg(all(unix, target_pointer_width = "64"))]
pub mod tuple_index;

pub use error::{Error, Result};
pub use record::{Link, LinkFields, LinkState, Linkage, Record, RecordSet, Uuid};
pub use record_log::{Compression, Entry, PersistentHeader, RecordLog, Tuple, Value};

// Compile and run the examples in the README.
#[cfg(doctest)]
#[doc = include_str!("../README.md")]
struct ReadmeDoctests;
