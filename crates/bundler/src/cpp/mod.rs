mod expander;

use crate::{
    bundler::{Bundle, Bundler},
    source::{Language, Source},
};
use expander::CppExpander;
use std::{error::Error, path::Path};

pub struct CppBundler {}

impl Bundler for CppBundler {
    /// Checks if a file is a C/C++ entrypoint
    fn is_entrypoint(path: &Path) -> bool {
        path.extension()
            .map(|ext| ext.to_ascii_lowercase())
            .is_some_and(|ext| ext == "cpp" || ext == "c")
    }

    fn priority(path: &Path) -> u8 {
        match path
            .file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .to_ascii_lowercase()
            .as_str()
        {
            // prefer main.cpp over other cpp files
            "main.cpp" => 10,
            "main.c" => 9,
            _ => match path
                .extension()
                .unwrap_or_default()
                .to_string_lossy()
                .to_ascii_lowercase()
                .as_str()
            {
                // prefer cpp over c
                "cpp" => 5,
                "c" => 4,
                _ => 0,
            },
        }
    }

    fn bundle(main_path: &Path) -> Result<Bundle, Box<dyn Error>> {
        assert!(Self::is_entrypoint(main_path));

        let mut expander = CppExpander::new();
        let source = expander.expand_source(main_path)?.ok_or("No source")?;

        Ok(Bundle {
            source: Source {
                code: format!("{}\n{}", PRAGMAS, source),
                language: Language::Cpp,
            },
            src_files: expander.files_included,
        })
    }
}

static PRAGMAS: &str = include_str!("pragmas.h");

#[cfg(test)]
mod tests {
    use super::*;
    use build_fs_tree::{dir, file, Build, FileSystemTree, MergeableFileSystemTree};
    use std::ffi::OsStr;
    use tempfile::TempDir;

    fn prepare_fixture(tree: FileSystemTree<&str, &str>) -> TempDir {
        let tmp = tempfile::tempdir().expect("tempdir");
        let tree: MergeableFileSystemTree<_, _> = MergeableFileSystemTree::from(tree);
        tree.build(tmp.path()).expect("build fs fixture");
        tmp
    }

    #[test]
    fn empty_dir_returns_none() {
        let tmp = prepare_fixture(dir! {});

        assert!(
            CppBundler::find_entrypoint(tmp.path()).is_none(),
            "empty directory should return None"
        );
        assert!(
            CppBundler::find_entrypoint(&tmp.path().join("non_existing.cpp")).is_none(),
            "non-existing file should return None"
        );
    }

    #[test]
    fn folder_finds_main_cpp() {
        let tmp = prepare_fixture(dir! {
            "main.cpp" => file!(""),
            "main.c" => file!(""),
            "source.cpp" => file!(""),
            "other.txt" => file!("")
        });
        let got = CppBundler::find_entrypoint(tmp.path());
        assert_eq!(
            got.as_ref().map(|p| p.file_name().unwrap()),
            Some(OsStr::new("main.cpp"))
        );
    }

    #[test]
    fn folder_finds_non_main_cpp() {
        let tmp = prepare_fixture(dir! {
            "source.cpp" => file!(""),
            "other.txt" => file!("")
        });
        let got = CppBundler::find_entrypoint(tmp.path());
        assert_eq!(
            got.as_ref().map(|p| p.file_name().unwrap()),
            Some(OsStr::new("source.cpp"))
        );
    }

    #[test]
    fn file_cpp() {
        let tmp = prepare_fixture(dir! {
            "main.cpp" => file!(""),
        });
        let got = CppBundler::find_entrypoint(&tmp.path().join("main.cpp"));
        assert_eq!(
            got.as_ref().map(|p| p.file_name().unwrap()),
            Some(OsStr::new("main.cpp"))
        );
    }
}
