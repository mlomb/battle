use crate::error::BundlerError;
use std::collections::HashSet;
use std::path::{Path, PathBuf};

pub struct CppExpander {
    /// Directories to search for included files (-I)
    include_dirs: Vec<PathBuf>,

    /// Files that have already been included
    pub files_included: HashSet<PathBuf>,
}

impl CppExpander {
    pub fn new() -> Self {
        Self {
            include_dirs: vec![],
            files_included: HashSet::new(),
        }
    }

    /// Expands the source file by resolving all the includes
    pub fn expand_source(&mut self, source_file: &Path) -> Result<Option<String>, BundlerError> {
        if self.has_been_included(source_file) {
            return Ok(None);
        }
        self.mark_as_included(source_file);

        let lines = std::fs::read_to_string(source_file)
            .map_err(|e| BundlerError::Io {
                path: source_file.to_path_buf(),
                error: e,
            })?
            .lines()
            .map(|line| self.process_line(source_file, line))
            .collect::<Result<Vec<_>, _>>()?
            .into_iter()
            .flatten()
            .collect::<Vec<_>>()
            .join("\n");

        Ok(Some(lines))
    }

    /// Processes a single line of the source file
    fn process_line(
        &mut self,
        source_file: &Path,
        line: &str,
    ) -> Result<Option<String>, BundlerError> {
        // we are looking for includes which contain a quoted path
        if line.trim().starts_with("#include \"") {
            let include_str = line.trim_start_matches("#include \"").trim_end_matches('"');
            let include_path = self.resolve_include(source_file, Path::new(include_str))?;

            return Ok(Some(match self.expand_source(&include_path)? {
                Some(inc_source) => format!("// {}\n{}", line, inc_source),
                None => format!("// (already included) {}", line),
            }));
        }

        // if line is a #pragma once, remove it
        if line.trim().starts_with("#pragma once") {
            return Ok(None);
        }

        // leave line unchanged
        Ok(Some(line.to_string()))
    }

    /// Resolves an include directive
    fn resolve_include(
        &mut self,
        source_file: &Path,
        include_path: &Path,
    ) -> Result<PathBuf, BundlerError> {
        // folders to search for the include file
        let mut candidates = self.include_dirs.clone();
        // the current source directory has priority
        candidates.insert(0, source_file.parent().unwrap().to_path_buf());

        // find the included file in the search paths, in the order provided
        candidates
            .iter()
            .map(|dir| dir.join(include_path))
            .find(|candidate| candidate.exists())
            .ok_or_else(|| BundlerError::Io {
                path: source_file.to_path_buf(),
                error: std::io::Error::new(
                    std::io::ErrorKind::NotFound,
                    format!(
                        "failed to resolve include: {} in file {}",
                        include_path.to_string_lossy(),
                        source_file.to_string_lossy()
                    ),
                ),
            })
    }

    fn mark_as_included(&mut self, include_path: &Path) {
        self.files_included
            .insert(std::path::absolute(include_path).unwrap());
    }

    fn has_been_included(&self, include_path: &Path) -> bool {
        self.files_included
            .contains(&std::path::absolute(include_path).unwrap())
    }
}

#[cfg(test)]
mod tests {
    use super::CppExpander;
    use build_fs_tree::{dir, file, Build, FileSystemTree, MergeableFileSystemTree};
    use tempfile::TempDir;

    fn prepare_fixture(tree: FileSystemTree<&str, &str>) -> TempDir {
        let tmp = tempfile::tempdir().expect("tempdir");
        let root = tmp.path();
        let tree: MergeableFileSystemTree<_, _> = MergeableFileSystemTree::from(tree);
        tree.build(root).expect("build fs fixture");
        tmp
    }

    #[test]
    fn expands_includes_and_handles_dedup() {
        let tmp = prepare_fixture(dir! {
            "point.h" => file!("#pragma once // comment\n\nstruct Point { int x; int y; };\n")
            "main.cpp" => file!(concat!(
                "#include <iostream>\n",
                "#include \"./point.h\"\n",
                "#include <vector>\n",
                "#include \"point.h\"\n",
                "#include <map>\n",
                "#include \"./point.h\"\n",
                "#include <string>\n",
                "int main() { return 0; }\n",
            ))
        });

        let mut expander = CppExpander::new();
        let out = expander
            .expand_source(&tmp.path().join("main.cpp"))
            .expect("expand main.cpp")
            .expect("expected source content");

        // make sure all std includes are present
        assert_eq!(out.matches("<iostream>").count(), 1);
        assert_eq!(out.matches("<vector>").count(), 1);
        assert_eq!(out.matches("<map>").count(), 1);
        assert_eq!(out.matches("<string>").count(), 1);

        assert_eq!(
            out.matches("struct Point").count(),
            1,
            "Point should be included only once"
        );

        assert!(
            !out.contains("#pragma once"),
            "#pragma once should be stripped"
        );

        assert_eq!(
            out.matches("already included").count(),
            2,
            "duplicate includes leave `already included` markers"
        );

        assert_eq!(
            expander.files_included.len(),
            2,
            "expected main.cpp and point.h to be tracked, got: {:?}",
            expander.files_included
        );

        // re-expanding an already-visited file should return None
        let second = expander
            .expand_source(&tmp.path().join("point.h"))
            .expect("re-expand main.cpp");
        assert!(
            second.is_none(),
            "expected None on already-included file, got: {:?}",
            second
        );
    }

    #[test]
    fn fails_on_missing_include() {
        let tmp = prepare_fixture(dir! {
            "exists.h" => file!("#pragma once\n\nstruct Point { int x; int y; };\n"),
            "main.cpp" => file!(concat!(
                "#include \"exists.h\"\n",
                "#include \"does_not_exist.h\"\n",
                "int main() { return 0; }\n",
            ))
        });

        let mut expander = CppExpander::new();
        let result = expander.expand_source(&tmp.path().join("main.cpp"));

        assert!(
            result.is_err(),
            "expected an error when including a missing header",
        );
    }

    #[test]
    fn deep_include_tree() {
        let tmp = prepare_fixture(dir! {
            "main.cpp" => file!(concat!(
                "#include \"a.h\"\n",
                "int main() { return 0; }\n",
            )),
            "a.h" => file!("#pragma once\n#include \"b.h\"\n"),
            "b.h" => file!("#pragma once\n#include \"c.h\"\n"),
            "c.h" => file!("#pragma once\n\nstruct Point { int x; int y; };\n"),
        });

        let mut expander = CppExpander::new();
        let out = expander
            .expand_source(&tmp.path().join("main.cpp"))
            .expect("expand main.cpp")
            .expect("expected source content");

        assert_eq!(out.matches("struct Point").count(), 1);
    }

    #[test]
    fn prevent_infinite_include_loop() {
        let tmp = prepare_fixture(dir! {
            "main.cpp" => file!(concat!(
                "#include \"b.h\"\n",
                "int main() { return 0; }\n",
            )),
            // a.h -> b.h -> c.h -> a.h
            "a.h" => file!("#pragma once\n#include \"b.h\"\n// A\n"),
            "b.h" => file!("#pragma once\n#include \"c.h\"\n// B\n"),
            "c.h" => file!("#pragma once\n#include \"a.h\"\n// C\n"),
        });

        let mut expander = CppExpander::new();
        let out = expander
            .expand_source(&tmp.path().join("main.cpp"))
            .expect("expand main.cpp")
            .expect("expected source content");

        // expects correct source
        assert!(out.contains("int main() { return 0; }"));
        assert!(out.contains("// A"));
        assert!(out.contains("// B"));
        assert!(out.contains("// C"));
    }
}
