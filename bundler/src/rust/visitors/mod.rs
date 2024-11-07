mod attribute_remover;
mod mod_inliner;
mod params;
mod test_remover;
mod use_trimmer;

use attribute_remover::AttributeRemover;
use cargo_metadata::camino::Utf8Path;
use mod_inliner::ModInliner;
use params::ParameterExpander;
use std::{collections::HashSet, error::Error, path::PathBuf};
use syn::{visit_mut::VisitMut, File};
use test_remover::TestRemover;
use use_trimmer::UseTrimmer;

/// Parses a source file and applies all the visitors to it
pub fn resolve_source(
    src_path: &Utf8Path,
    lib_package_name: Option<String>,
    src_files: &mut HashSet<PathBuf>,
) -> Result<File, Box<dyn Error>> {
    let mut mod_inliner = ModInliner::new();
    let mut file = mod_inliner.resolve(src_path.as_std_path())?;

    if mod_inliner.unresolved_mods.len() > 0 {
        return Err(format!(
            "Failed to resolve mods: {}",
            mod_inliner
                .unresolved_mods
                .into_iter()
                .collect::<Vec<_>>()
                .join(", ")
        )
        .into());
    }

    TestRemover::new().visit_file_mut(&mut file);

    ParameterExpander::new().visit_file_mut(&mut file);

    AttributeRemover::new()
        // remove comments
        .with_attribute("doc")
        // remove WASM bindings
        .with_attribute("wasm_bindgen")
        .visit_file_mut(&mut file);

    if let Some(lib_package_name) = lib_package_name {
        UseTrimmer::with_prefix(lib_package_name).visit_file_mut(&mut file);
    }

    src_files.extend(mod_inliner.visited_files);

    Ok(file)
}
