mod attribute_remover;
mod mod_inliner;
mod params;

use attribute_remover::AttributeRemover;
use cargo_metadata::camino::Utf8Path;
use mod_inliner::ModInliner;
use params::ParameterExpander;
use std::{error::Error, path::PathBuf};
use syn::{visit_mut::VisitMut, File};

/// Parses a source file and applies all the visitors to it
pub fn resolve_source(
    src_path: &Utf8Path,
    src_files: &mut Vec<PathBuf>,
) -> Result<File, Box<dyn Error>> {
    let mut mod_inliner = ModInliner::new();
    let mut file = mod_inliner.resolve(src_path.as_std_path())?;

    src_files.extend(mod_inliner.visited_files);

    ParameterExpander::new().visit_file_mut(&mut file);

    AttributeRemover::new()
        // remove comments
        .with_attribute("doc")
        // remove WASM bindings
        .with_attribute("wasm_bindgen")
        .visit_file_mut(&mut file);

    Ok(file)
}
