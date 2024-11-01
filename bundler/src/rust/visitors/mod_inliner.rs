use std::{
    collections::HashSet,
    error::Error,
    fs,
    path::{Path, PathBuf},
};
use syn::{parse_quote, visit_mut::VisitMut};

/// Recursively resolves the `use` and `extern crate` statements in the code,
/// effectively inlining all the code.
///
/// At the same time, it collecs all the files that were read to resolve the lines.
pub struct ModInliner {
    /// The current file path being resolved (e.g. lib.rs)
    current_file_path: Option<PathBuf>,

    /// Every Rust file that was visited to build the final syn::File
    visited_files: HashSet<PathBuf>,
}

impl ModInliner {
    pub fn new() -> Self {
        Self {
            current_file_path: None,
            visited_files: HashSet::new(),
        }
    }
}

impl ModInliner {
    pub fn resolve(&mut self, rust_file: impl AsRef<Path>) -> Result<syn::File, Box<dyn Error>> {
        // convert to PathBuf
        let rust_file = rust_file.as_ref().to_path_buf();

        // load source from disk
        let source_code = fs::read_to_string(&rust_file)?;

        // parse into syn::File
        let mut file = syn::parse_file(&source_code).map_err(|e| {
            format!(
                "failed to parse file {} @ Ln {} Col {}: {}",
                rust_file.file_name().unwrap().to_str().unwrap(),
                e.span().start().line,
                e.span().start().column,
                e
            )
        })?;

        // add to the list of visited files (so they can be watched later)
        self.visited_files.insert(rust_file.clone());

        // store the current file path
        let prev_file_path = self.current_file_path.clone();
        self.current_file_path = Some(rust_file);

        self.visit_file_mut(&mut file);

        // restore the previous file path
        self.current_file_path = prev_file_path;

        Ok(file)
    }
}

impl VisitMut for ModInliner {
    fn visit_item_mod_mut(&mut self, i: &mut syn::ItemMod) {
        // check that the mod is not defined inline (not mod m { ... })
        if i.content.is_none() {
            let current_file_path = self.current_file_path.clone().unwrap();
            let mod_name = i.ident.to_string();

            // possible locations for the mod file
            let candidate_locations = vec![
                // mod_name.rs
                current_file_path
                    .parent()
                    .unwrap()
                    .join(&mod_name)
                    .with_extension("rs"),
                // mod_name/mod.rs
                current_file_path
                    .parent()
                    .unwrap()
                    .join(&mod_name)
                    .join("mod.rs"),
            ];

            // resolve mod recursively
            let file = candidate_locations
                .into_iter()
                .flat_map(|rust_file| self.resolve(rust_file))
                .next()
                .ok_or(format!("mod '{}' not found!", mod_name));

            if let Ok(mut file) = file {
                self.visit_file_mut(&mut file);

                // Note: file attributes are being dropped (shebang)
                i.content = Some((Default::default(), file.items));
            } else {
                i.attrs.push(parse_quote! { #[doc="Failed to resolve"] });
                i.content = Some((Default::default(), vec![]));
            }
        }
    }

    fn visit_item_extern_crate_mut(&mut self, i: &mut syn::ItemExternCrate) {
        // let code =
        //     fs::read_to_string(&self.base_path.join("lib.rs")).expect("failed to read lib.rs");
        // let lib = syn::parse_file(&code).expect("failed to parse lib.rs");
        // new_items.extend(lib.items);
    }

    fn visit_item_use_mut(&mut self, i: &mut syn::ItemUse) {
        //
        println!("visiting use");
    }
}
