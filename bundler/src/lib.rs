extern crate cargo_metadata;
extern crate quote;
extern crate syn;

mod bundler;
mod cpp;
mod rust;

use bundler::Bundler;
use cpp::CppBundler;
use rust::RustBundler;
use std::{error::Error, path::Path};

enum Language {
    Cpp,
    Rust,
}

struct Bundle {
    lang: Language,
    source: String,
}

pub fn bundle(entry: &Path) -> Result<String, Box<dyn Error>> {
    let bundler = RustBundler::new();

    let r = CppBundler::find_entrypoint(entry);
    let r2 = RustBundler::find_entrypoint(entry);

    RustBundler::bundle(r2.ok_or("mp oteas")?.as_path())
}
