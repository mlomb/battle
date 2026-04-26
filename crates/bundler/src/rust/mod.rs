mod format;
mod visitors;

use crate::bundler::{Bundle, Bundler};
use crate::source::{Language, Source};
use cargo_metadata::MetadataCommand;
use format::{format_code, FmtError};
use quote::quote;
use std::collections::{HashMap, HashSet};
use std::error::Error;
use std::path::Path;
use visitors::resolve_source;

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
            .filter(|target| target.kind.iter().any(|t| t.contains("lib"))) // lib, rlib, cdylib
            .next();

        let mut src_files = HashSet::new();
        src_files.insert(package.manifest_path.to_path_buf().into());

        let mut target_file = resolve_source(
            &target.src_path,
            // pass the name of the package so `use` statements are trimmed
            // `use pkg::foo` -> `use foo`;
            Some(package.name.clone()),
            &mut src_files,
        )?;

        if let Some(lib) = lib {
            let lib_file = resolve_source(&lib.src_path, None, &mut src_files)?;

            target_file.attrs.splice(..0, lib_file.attrs);
            target_file.items.splice(..0, lib_file.items);
            target_file.shebang = target_file.shebang.or(lib_file.shebang);
        }

        let source = quote!(#target_file).to_string();

        match format_code(&source) {
            Ok(source) => {
                let source = source.replace("use wasm_bindgen::prelude::*;", "");

                Ok(Bundle {
                    source: Source {
                        code: source,
                        language: Language::Rust,
                    },
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
