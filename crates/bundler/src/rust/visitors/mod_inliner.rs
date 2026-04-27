use std::{
    collections::HashSet,
    error::Error,
    fs,
    path::{Path, PathBuf},
};
use syn::{parse_quote, visit_mut::VisitMut};

/// Recursively inlines `mod` statements.
///
/// At the same time, it collecs all the files that were read.
pub struct ModInliner {
    /// The current file path being resolved (e.g. src/lib.rs)
    current_file_path: Option<PathBuf>,

    /// Every Rust file that was visited
    pub(crate) visited_files: HashSet<PathBuf>,
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

            match file {
                Ok(file) => {
                    // Note: file attributes are being dropped (shebangs #!)
                    i.content = Some((Default::default(), file.items));
                    i.semi = None;
                }
                Err(err) => {
                    let msg = format!("Failed to resolve: {}", err);
                    i.attrs.push(parse_quote! { #[doc=#msg] });
                    // Note: content is empty (no { ... })
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::ModInliner;
    use build_fs_tree::{dir, file, Build, FileSystemTree, MergeableFileSystemTree};
    use syn::File;
    use tempfile::TempDir;

    fn prepare_fixture(tree: FileSystemTree<&str, &str>) -> TempDir {
        let tmp = tempfile::tempdir().expect("tempdir");
        let tree: MergeableFileSystemTree<_, _> = MergeableFileSystemTree::from(tree);
        tree.build(tmp.path()).expect("build fs fixture");
        tmp
    }

    #[test]
    fn inline_file_backed_module() {
        let tmp = prepare_fixture(dir! {
            "lib.rs" => file!(r#"
            mod child;
            mod inline {
                pub fn already_here() {}
            }

            pub fn root_fn() {}
            "#),
            "child.rs" => file!("pub struct InChild;"),
        });
        let root = tmp.path().join("lib.rs");

        let mut inliner = ModInliner::new();
        let got = inliner.resolve(&root).expect("resolve");

        let expected: File = syn::parse_str(
            r#"
            mod child {
                pub struct InChild;
            }
            mod inline {
                pub fn already_here() {}
            }

            pub fn root_fn() {}
            "#,
        )
        .expect("parse expected");

        assert_eq!(got, expected);
    }

    /// Second candidate path: `name/mod.rs` when `name.rs` does not exist.
    #[test]
    fn resolves_module_via_mod_rs_subdir() {
        let tmp = prepare_fixture(dir! {
            "lib.rs" => file!("mod nested;"),
            "nested" => dir! {
                "mod.rs" => file!("pub struct FromModRs;"),
            },
        });

        let mut inliner = ModInliner::new();
        let got = inliner.resolve(tmp.path().join("lib.rs")).expect("resolve");

        let expected: File = syn::parse_str(
            r#"
            mod nested {
                pub struct FromModRs;
            }
            "#,
        )
        .expect("parse expected");

        assert_eq!(got, expected);
    }

    #[test]
    fn unresolved_module_gets_doc_attribute() {
        let tmp = prepare_fixture(dir! {
            "lib.rs" => file!(r#"
            mod nope;

            pub fn ok() {}
            "#),
        });

        let mut inliner = ModInliner::new();
        let got = inliner.resolve(tmp.path().join("lib.rs")).expect("resolve");

        let expected: File = syn::parse_str(
            r#"
            #[doc = "Failed to resolve: mod 'nope' not found!"]
            mod nope;

            pub fn ok() {}
            "#,
        )
        .expect("parse expected");

        assert_eq!(got, expected);
    }

    #[test]
    fn resolve_propagates_parse_errors() {
        let tmp = prepare_fixture(dir! {
            "lib.rs" => file!("this is not valid rust"),
        });

        let mut inliner = ModInliner::new();
        inliner
            .resolve(tmp.path().join("lib.rs"))
            .expect_err("invalid source should fail");
    }
}
