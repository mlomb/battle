mod format;
mod visitors;

use crate::bundler::{Bundle, Bundler};
use crate::error::BundlerError;
use crate::source::{Language, Source};
use cargo_metadata::{MetadataCommand, TargetKind};
use format::{format_code, FmtError};
use quote::quote;
use std::collections::{HashMap, HashSet};
use std::path::Path;
use syn::visit_mut::VisitMut;
use visitors::{resolve_source, ConstOverrider};

pub struct RustBundler {}

impl Bundler for RustBundler {
    /// Check if the path points to a valid Cargo.toml file
    fn is_entrypoint(path: &Path) -> bool {
        path.file_name()
            .unwrap_or_default()
            .to_ascii_lowercase()
            .eq("cargo.toml")
    }

    fn bundle(manifest_path: &Path, defines: &[String]) -> Result<Bundle, BundlerError> {
        assert!(Self::is_entrypoint(manifest_path));

        let metadata = MetadataCommand::new()
            .manifest_path(manifest_path)
            // .features(CargoOpt::AllFeatures)
            .exec()
            .map_err(|e| BundlerError::Other(format!("failed to read Cargo.toml: {}", e)))?;

        let package = metadata.root_package().ok_or(BundlerError::Other(
            "cargo project has no root package".to_string(),
        ))?;

        // take the first occurrence of a binary target as the entry point
        let target = package
            .targets
            .iter()
            .find(|target| target.kind.iter().any(|t| matches!(t, TargetKind::Bin)))
            .ok_or(BundlerError::Other(
                "cargo project has no binary target".to_string(),
            ))?;

        // check if package has a lib
        // packages can only have one lib
        let lib = package.targets.iter().find(|target| {
            target
                .kind
                .iter()
                .any(|t| matches!(t, TargetKind::Lib | TargetKind::RLib | TargetKind::CDyLib))
        });

        let mut src_files = HashSet::new();
        src_files.insert(package.manifest_path.to_path_buf().into());

        let mut target_file = resolve_source(
            &target.src_path,
            // pass the name of the package so `use` statements are trimmed
            // `use pkg::foo` -> `use foo`;
            Some(package.name.to_string()),
            &mut src_files,
        )?;

        if let Some(lib) = lib {
            let lib_file = resolve_source(&lib.src_path, None, &mut src_files)?;

            target_file.attrs.splice(..0, lib_file.attrs);
            target_file.items.splice(..0, lib_file.items);
            target_file.shebang = target_file.shebang.or(lib_file.shebang);
        }

        // `NAME=VALUE` sets the value of `const NAME`
        let mut values = HashMap::new();
        for define in defines {
            let (name, value) = define.split_once('=').unwrap_or((define, "true"));
            let value = syn::parse_str(value)
                .map_err(|e| BundlerError::Other(format!("invalid value for {name}: {e}")))?;
            values.insert(name.to_string(), value);
        }
        ConstOverrider(values).visit_file_mut(&mut target_file);

        let source = quote!(#target_file).to_string();

        match format_code(&source) {
            Ok(source) => {
                let source = source.replace("use wasm_bindgen::prelude::*;", "");

                Ok(Bundle {
                    source: Source {
                        code: source,
                        language: Language::Rust,
                    },
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
