mod format;
mod visitors;

use crate::bundler::{Bundle, Bundler};
use cargo_metadata::MetadataCommand;
use format::{format_code, FmtError};
use quote::quote;
use std::collections::HashMap;
use std::error::Error;
use std::fs;
use std::path::Path;
use syn::visit_mut::VisitMut;
use visitors::attribute_remover::AttributeRemover;
use visitors::mod_inliner::ModInliner;
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

        // take the first occurrence of a binary target as the entry point
        let package = metadata.root_package().unwrap();
        let target = package
            .targets
            .iter()
            .find(|target| target.kind.iter().any(|t| t == "bin"))
            .expect("no binary target found");

        let content = fs::read_to_string(target.src_path.as_std_path())?;

        let mut file = syn::parse_file(&content).map_err(|e| {
            format!(
                "failed to parse file {} @ Ln {} Col {}: {}",
                target.src_path.file_name().unwrap(),
                e.span().start().line,
                e.span().start().column,
                e
            )
        })?;

        println!("deps: {:?}", package.dependencies);

        ModInliner {
            base_path: package.manifest_path.parent().unwrap().join("src").into(),
            crate_name: package.name.replace("-", "_"),
        }
        .visit_file_mut(&mut file);

        ParameterExpander {}.visit_file_mut(&mut file);

        AttributeRemover::new()
            // remove comments
            .with_attribute("doc")
            // remove WASM bindings
            .with_attribute("wasm_bindgen")
            .visit_file_mut(&mut file);

        let source = quote!(#file).to_string();

        match format_code(&source) {
            Ok(source) => {
                let source = source.replace("use wasm_bindgen::prelude::*;", "");

                Ok(Bundle {
                    source,
                    params: HashMap::new(),
                    files: vec![],
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
