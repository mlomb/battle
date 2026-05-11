use std::{fmt, path::PathBuf};

/// Errors produced when bundling a project into a single source unit.
#[derive(Debug)]
pub enum BundlerError {
    /// No Cargo.toml or C/C++ root file matched the requested entry path.
    NoEntrypoint,

    /// A syntax error occurred while parsing a file.
    Syntax {
        path: PathBuf,
        line: usize,
        column: usize,
        error: String,
    },

    /// An I/O error occurred while reading a file.
    Io {
        path: PathBuf,
        error: std::io::Error,
    },

    /// Some other error occurred while bundling the project.
    Other(String),
}

impl fmt::Display for BundlerError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            BundlerError::NoEntrypoint => write!(f, "no entrypoint found"),
            BundlerError::Io { path, error } => {
                write!(f, "{}: {error}", path.display())
            }
            BundlerError::Other(msg) => write!(f, "{msg}"),
            // "failed to parse file {} @ Ln {} Col {}: {}"
            BundlerError::Syntax {
                path,
                error,
                line,
                column,
            } => {
                write!(
                    f,
                    "failed to parse file {} @ Ln {} Col {}: {}",
                    path.display(),
                    line,
                    column,
                    error
                )
            }
        }
    }
}
