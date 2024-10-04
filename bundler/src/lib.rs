#[macro_use]
extern crate quote;
extern crate cargo_metadata;
extern crate syn;

mod cleaner;
mod expander;
mod format;

use crate::format::format_code;
use cargo_metadata::MetadataCommand;
use cleaner::Cleaner;
use expander::Expander;
use format::FmtError;
use std::{error::Error, path::Path};
use std::{fs::File, io::Read};
use syn::visit_mut::VisitMut;

#[derive(Debug)]
pub struct Bundler {
    pub package_path: Path,
}

pub fn bundle<P: AsRef<Path>>(package_path: P) -> Result<String, Box<dyn Error>> {
    let metadata = MetadataCommand::new()
        .manifest_path(package_path.as_ref().join("Cargo.toml"))
        // .features(CargoOpt::AllFeatures)
        .exec()?;

    let package = metadata.root_package().unwrap();
    let target = &package.targets[1];

    let content = read_file(target.src_path.as_std_path())?;

    let mut file = syn::parse_file(&content)?; //.expect("failed to parse binary target source");

    Expander {
        base_path: &package_path.as_ref().join("src"),
        crate_name: &package.name.replace("-", "_"),
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

fn read_file(path: &Path) -> Result<String, std::io::Error> {
    let mut file = File::open(path)?;
    let mut contents = String::new();
    file.read_to_string(&mut contents)?;
    Ok(contents)
}
