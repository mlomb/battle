use std::{
    collections::HashSet,
    error::Error,
    path::{Path, PathBuf},
};

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
    pub fn expand_source(&mut self, source_file: &Path) -> Result<Option<String>, Box<dyn Error>> {
        if self.has_been_included(&source_file) {
            return Ok(None);
        }
        self.mark_as_included(source_file);

        let lines = std::fs::read_to_string(source_file)?
            .lines()
            .into_iter()
            .map(|line| self.process_line(source_file, line))
            .collect::<Result<Vec<_>, _>>()?
            .into_iter()
            .filter_map(|line| line)
            .collect::<Vec<_>>()
            .join("\n");

        Ok(Some(lines))
    }

    /// Processes a single line of the source file
    fn process_line(
        &mut self,
        source_file: &Path,
        line: &str,
    ) -> Result<Option<String>, Box<dyn Error>> {
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
        if line.trim() == "#pragma once" {
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
    ) -> Result<PathBuf, String> {
        // folders to search for the include file
        let mut candidates = self.include_dirs.clone();
        // the current source directory has priority
        candidates.insert(0, source_file.parent().unwrap().to_path_buf());

        // find the included file in the search paths, in the order provided
        candidates
            .iter()
            .map(|dir| dir.join(include_path))
            .filter(|candidate| candidate.exists())
            .next()
            .ok_or(format!(
                "Failed to resolve include: {} in file: {}",
                include_path.to_str().unwrap(),
                source_file.to_str().unwrap()
            ))
    }

    fn mark_as_included(&mut self, include_path: &Path) {
        self.files_included
            .insert(std::fs::canonicalize(include_path).unwrap());
    }

    fn has_been_included(&self, include_path: &Path) -> bool {
        self.files_included
            .contains(&std::fs::canonicalize(include_path).unwrap())
    }
}
