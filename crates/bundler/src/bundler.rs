use crate::{cpp::CppBundler, error::BundlerError, rust::RustBundler, source::Source, BundlerArgs};
use std::{
    cmp::Reverse,
    collections::HashSet,
    path::{Path, PathBuf},
};

/// The trait all bundlers implement
pub trait Bundler {
    /// Checks if the path leads to a valid entry point
    fn is_entrypoint(path: &Path) -> bool;

    /// Returns the priority of the entrypoint file. Higher number is preferred.
    fn priority(_path: &Path) -> u8 {
        0
    }

    /// Bundles the project into a single source unit
    fn bundle(path: &Path) -> Result<Bundle, BundlerError>;

    /// Find the entrypoint file of a project
    fn find_entrypoint(path: &Path) -> Option<PathBuf> {
        // enumerate available files
        let mut candidates = if path.is_dir() {
            path.read_dir()
                .expect("failed to read directory")
                .filter_map(|entry| entry.ok())
                .map(|entry| entry.path())
                .collect::<Vec<_>>()
        } else {
            vec![path.to_path_buf()]
        };

        // filter out invalid entrypoint files
        candidates.retain(|path| Self::is_entrypoint(path));

        // only retain existing files
        candidates.retain(|path| path.exists());

        // sort by priority (higher first)
        candidates.sort_by_key(|path| Reverse(Self::priority(path)));

        // return the first valid entrypoint file
        candidates.first().map(|p| p.to_path_buf())
    }
}

/// The result of bundling a project
#[derive(Debug)]
pub struct Bundle {
    /// The bundled source code
    pub source: Source,

    /// Parameters found in the original source. Now available to set via standard arguments
    // pub params: HashMap<String, Parameter>,

    /// All relevant files used to create the bundle (and should be watched)
    // TODO: change to just files, since not every file might be source code
    pub src_files: HashSet<PathBuf>,
}

/// Bundles a C++/Rust project directory into a single source unit
pub fn bundle(args: &BundlerArgs) -> Result<Bundle, BundlerError> {
    let entry = args.entry.clone().unwrap_or_else(|| PathBuf::from("."));

    if let Some(entry) = RustBundler::find_entrypoint(entry.as_path()) {
        return RustBundler::bundle(entry.as_path());
    }

    if let Some(entry) = CppBundler::find_entrypoint(entry.as_path()) {
        return CppBundler::bundle(entry.as_path());
    }

    Err(BundlerError::NoEntrypoint)
}
