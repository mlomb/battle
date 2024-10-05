mod cleaner;
mod expander;
mod format;

use crate::bundler::Bundler;
use cargo_metadata::MetadataCommand;
use cleaner::Cleaner;
use expander::Expander;
use format::{format_code, FmtError};
use quote::quote;
use std::error::Error;
use std::path::Path;
use std::{fs::File, io::Read};
use syn::visit_mut::VisitMut;

pub struct RustBundle {
    source: String,
}

pub struct RustBundler {}

impl RustBundler {
    pub fn new() -> Self {
        Self {}
    }
}

impl Bundler for RustBundler {
    /// Check if the path points to a valid Cargo.toml file
    fn is_entrypoint(path: &Path) -> bool {
        path.file_name()
            .unwrap_or_default()
            .to_ascii_lowercase()
            .eq("cargo.toml")
    }

    fn bundle(manifest_path: &Path) -> Result<String, Box<dyn Error>> {
        let metadata = MetadataCommand::new()
            .manifest_path(manifest_path)
            // .features(CargoOpt::AllFeatures)
            .exec()?;

        let package = metadata.root_package().unwrap();
        let target = package
            .targets
            .iter()
            .find(|target| target.kind.iter().any(|t| t == "bin"))
            .expect("no binary target found");

        let content = read_file(target.src_path.as_std_path())?;

        let mut file = syn::parse_file(&content)?; //.expect("failed to parse binary target source");

        Expander {
            base_path: manifest_path.parent().unwrap().join("src"),
            crate_name: package.name.replace("-", "_"),
        }
        .visit_file_mut(&mut file);

        Cleaner {
            attributes_to_remove: vec!["doc".to_string(), "wasm_bindgen".to_string()],
        }
        .visit_file_mut(&mut file);

        let source = quote!(#file).to_string();

        match format_code(&source) {
            Ok(source) => {
                let source = source.replace("use wasm_bindgen::prelude::*;", "");

                Ok(source)
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

fn read_file(path: &Path) -> Result<String, std::io::Error> {
    let mut file = File::open(path)?;
    let mut contents = String::new();
    file.read_to_string(&mut contents)?;
    Ok(contents)
}
