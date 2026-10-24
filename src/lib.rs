//! gdblite: a lightweight graph database.
//!
//! The storage layer is an append-only [`RecordLog`] of entries addressed by
//! their byte position (their *id*).  An entry is either a [`Value`] (a byte
//! blob; an empty value is a bare node) or a [`Tuple`] (an ordered list of ids
//! linking earlier entries together).

pub mod coder;
mod error;
pub mod record_log;

pub use error::{Error, Result};
pub use record_log::{Compression, Entry, PersistentHeader, RecordLog, Tuple, Value};

// Compile and run the examples in the README.
#[cfg(doctest)]
#[doc = include_str!("../README.md")]
struct ReadmeDoctests;
