use super::{command::ToCommand, executable::ExecutableError};
use log::info;
use serde::{Deserialize, Serialize};
use std::{
    collections::HashMap,
    fmt::{self, Formatter},
    path::PathBuf,
    process::Command,
};
use tempfile::TempDir;

/// A command that can be executed. It includes all the necessary files to run it.
#[derive(Serialize, Deserialize)]
pub struct ExecutableCommand {
    /// The base command to execute.
    ///
    /// For example:
    ///   - `["python3", "main.py"]`
    ///   - `["./main"]`
    ///   - `["java", "-jar", "main.jar"]`
    command: Vec<String>,

    /// Required files for the execution (like the binary itself).
    /// Can also include additional assets.
    ///
    /// For example:
    ///   - `{"main.py": b"print('Hello, World!')"}`
    ///   - `{"main": b"\x7fELF..."}` (binary)
    ///   - `{"main.jar": b"PK..."}` (jar)
    files: HashMap<String, Vec<u8>>,

    /// Current directory where files are located in disk.
    /// It is lazily initialized when `command` is called.
    /// It is reused across executions.
    /// Upon drop, the directory is removed.
    #[serde(skip)]
    tmp_workdir: Option<TempDir>,
}

impl ExecutableCommand {
    /// Creates a new executable from a command prefix and a file.
    ///
    /// # Example
    /// ```no_run
    /// Executable::from_prefix_file(
    ///     vec!["java".to_string(), "-jar".to_string()],
    ///     PathBuf::from("main.jar")
    /// );
    /// ```
    pub fn from_prefix_file(
        command_prefix: Vec<String>,
        file: PathBuf,
    ) -> Result<Self, ExecutableError> {
        let filename = file
            .file_name()
            .ok_or_else(|| ExecutableError::InvalidFileName(file.clone()))?
            .to_string_lossy()
            .to_string();

        let content =
            std::fs::read(&file).map_err(|_| ExecutableError::FileNotFound(file.clone()))?;

        let mut files = HashMap::new();
        files.insert(filename.clone(), content);

        Ok(Self {
            command: [command_prefix, vec![filename]].concat(),
            files,
            tmp_workdir: None,
        })
    }

    /// Creates a new executable that runs a JAR file ("java -jar main.jar")
    pub fn from_jar(jar_path: PathBuf) -> Result<Self, ExecutableError> {
        Self::from_prefix_file(
            vec![
                "java".to_string(),
                // This is due CodinGame engine accessing internal classes which is not supported in modern Java
                "--add-opens java.base/java.lang=ALL-UNNAMED".to_string(),
                "-jar".to_string(),
            ],
            jar_path,
        )
    }

    /// Creates a new executable that runs a binary file (e.g. "main.exe")
    pub fn from_binary(binary_path: PathBuf) -> Result<Self, ExecutableError> {
        Self::from_prefix_file(vec![], binary_path)
    }

    /// Initializes the temporal directory and writes the files to it
    fn initialize_files(&mut self) -> Result<(), ExecutableError> {
        if self.tmp_workdir.is_none() {
            info!(
                "Initializing executable {}, files: {:?} ({} bytes)",
                self.command[0],
                self.files.keys(),
                self.files.values().map(|v| v.len()).sum::<usize>()
            );

            let tmp = TempDir::new()
                .map_err(|io_err| ExecutableError::TempDirFailed(io_err.to_string()))?;

            // write files to disk
            for (name, content) in &self.files {
                let path = tmp.path().join(name);
                std::fs::write(&path, content)
                    .map_err(|io_err| ExecutableError::WriteFileFailed(io_err.to_string()))?;
            }

            self.tmp_workdir = Some(tmp);
        }

        Ok(())
    }
}

impl ToCommand for ExecutableCommand {
    fn command(&mut self) -> Result<Command, ExecutableError> {
        // populate tmp_workdir
        self.initialize_files()?;

        let mut cmd = Command::new(&self.command[0]);
        cmd.args(&self.command[1..]);
        cmd.current_dir(self.tmp_workdir.as_ref().expect("files available").path());
        Ok(cmd)
    }
}

impl Clone for ExecutableCommand {
    fn clone(&self) -> Self {
        Self {
            command: self.command.clone(),
            files: self.files.clone(),
            tmp_workdir: None,
        }
    }
}

impl fmt::Debug for ExecutableCommand {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Executable")
            .field("command", &self.command)
            .field("files", &self.files.keys())
            .field("tmp_workdir", &self.tmp_workdir)
            .finish()
    }
}
