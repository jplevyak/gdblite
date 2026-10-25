use std::fmt;

/// Errors produced while encoding or decoding a record log.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    /// The input ended before a complete item could be decoded.
    Truncated,
    /// The output buffer has no room for the encoded item.
    BufferTooSmall,
    /// A decoded integer does not fit in 64 bits, or an id is too large to
    /// encode (ids must be at most `i64::MAX`).
    Overflow,
    /// The id does not address a position inside the log body.
    IdOutOfRange,
    /// The entry is a tuple where a value was expected, or vice versa.
    KindMismatch,
    /// A link field refers to an id at or after the link itself.
    ForwardReference,
}

pub type Result<T> = std::result::Result<T, Error>;

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Error::Truncated => "input truncated",
            Error::BufferTooSmall => "output buffer too small",
            Error::Overflow => "integer overflow",
            Error::IdOutOfRange => "id out of range",
            Error::KindMismatch => "entry kind mismatch",
            Error::ForwardReference => "link refers forward",
        })
    }
}

impl std::error::Error for Error {}
