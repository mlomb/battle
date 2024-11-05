mod format;
mod visitors;

use crate::bundler::{Bundle, Bundler};
use cargo_metadata::camino::Utf8Path;
use cargo_metadata::MetadataCommand;
use format::{format_code, FmtError};
use quote::quote;
use std::collections::HashMap;
use std::error::Error;
use std::path::{Path, PathBuf};
use syn::visit_mut::VisitMut;
use syn::File;
use visitors::attribute_remover::AttributeRemover;
use visitors::mod_inliner::{self, ModInliner};
use visitors::params::ParameterExpander;

pub struct RustBundler {}

impl Bundler for RustBundler {
    /// Check if the path points to a valid Cargo.toml file
    fn is_entrypoint(path: &Path) -> bool {
        path.file_name()
            .unwrap_or_default()
            .to_ascii_lowercase()
            .eq("cargo.toml")
    }

    fn bundle(manifest_path: &Path) -> Result<Bundle, Box<dyn Error>> {
        assert!(Self::is_entrypoint(manifest_path));

        let metadata = MetadataCommand::new()
            .manifest_path(manifest_path)
            // .features(CargoOpt::AllFeatures)
            .exec()?;
        let package = metadata.root_package().expect("no root package found");

        // take the first occurrence of a binary target as the entry point
        let target = package
            .targets
            .iter()
            .find(|target| target.kind.iter().any(|t| t == "bin"))
            .expect("no binary target found");

        // check if package has a lib
        // packages can only have one lib
        let lib = package
            .targets
            .iter()
            .filter(|target| target.kind.iter().any(|t| t == "lib"))
            .next();

        let mut src_files = vec![manifest_path.to_path_buf()];

        let mut target_file = resolve_source(&target.src_path, &mut src_files)?;

        if let Some(lib) = lib {
            let lib_file = resolve_source(&lib.src_path, &mut src_files)?;

            target_file.attrs.splice(..0, lib_file.attrs);
            target_file.items.splice(..0, lib_file.items);
            target_file.shebang = target_file.shebang.or(lib_file.shebang);
        }

        let source = quote!(#target_file).to_string();

        match format_code(&source) {
            Ok(source) => {
                let source = source.replace("use wasm_bindgen::prelude::*;", "");

                Ok(Bundle {
                    source,
                    params: HashMap::new(),
                    src_files,
                })
            }
            Err(FmtError::RustfmtNotFound) => {
                panic!("rustfmt component not found in toolchain (rustup component add rustfmt)");
            }
            Err(FmtError::Failed(msg)) => {
                println!("{}", msg);
                panic!("failed to format code");
            }
        }
    }
}

fn resolve_source(
    src_path: &Utf8Path,
    src_files: &mut Vec<PathBuf>,
) -> Result<File, Box<dyn Error>> {
    let mut mod_inliner = ModInliner::new();
    let mut file = mod_inliner.resolve(src_path.as_std_path())?;

    src_files.extend(mod_inliner.visited_files);

    ParameterExpander {}.visit_file_mut(&mut file);

    AttributeRemover::new()
        // remove comments
        .with_attribute("doc")
        // remove WASM bindings
        .with_attribute("wasm_bindgen")
        .visit_file_mut(&mut file);

    Ok(file)
}
