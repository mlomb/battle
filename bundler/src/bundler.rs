use std::{
    error::Error,
    path::{Path, PathBuf},
};

/// The trait all bundlers implement
pub trait Bundler {
    /// Checks if the path leads to a valid entry point
    fn is_entrypoint(path: &Path) -> bool;

    /// Bundles the project into a single source file
    fn bundle(path: &Path) -> Result<String, Box<dyn Error>>;

    /// Find the entrypoint file of a project
    fn find_entrypoint(path: &Path) -> Option<PathBuf> {
        // enumerate available files
        let mut candidates = if path.is_dir() {
            path.read_dir()
                .expect("failed to read directory")
                .filter_map(|entry| entry.ok())
                .filter_map(|entry| Some(entry.path()))
                .collect::<Vec<_>>()
        } else {
            vec![path.to_path_buf()]
        };

        // filter out entrypoint files
        candidates.retain(|path| Self::is_entrypoint(path));

        // return the first candidate
        candidates.first().map(|p| p.to_path_buf())
    }
}
