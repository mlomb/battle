use serde::Serialize;
use std::{collections::HashMap, path::PathBuf, process::Command};
use tempfile::TempDir;

/// Represents an arbitrary executable.
#[derive(Serialize)]
pub struct Executable {
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
    #[serde(skip_serializing)]
    tmp_workdir: Option<TempDir>,
}

impl Executable {
    /// Returns a ready-to-execute command.
    /// One may include additional arguments to the command.
    pub fn command(&mut self) -> Command {
        // populate tmp_workdir
        self.initialize_files();

        let mut cmd = Command::new(&self.command[0]);
        cmd.args(&self.command[1..]);
        cmd.current_dir(self.tmp_workdir.as_ref().expect("files available").path());
        cmd
    }

    /// Initializes the temporal directory and writes the files to it.  
    fn initialize_files(&mut self) {
        let tmp = TempDir::new().expect("failed to create temporary directory");

        // write files to disk
        for (name, content) in &self.files {
            let path = tmp.path().join(name);
            std::fs::write(&path, content).expect("failed to write file");
        }

        self.tmp_workdir = Some(tmp);
        println!(
            "Files available in {:?}",
            self.tmp_workdir.as_ref().unwrap().path()
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_command() {
        let mut exec = Executable {
            command: vec!["python".to_string(), "hello.py".to_string()],
            files: HashMap::from([("hello.py".to_string(), b"print(123,end='')".to_vec())]),
            tmp_workdir: None,
        };

        let mut cmd = exec.command();
        println!("{:?}", cmd);
        let output = cmd.output();
        println!("{:?}", output);
        assert!(output.is_ok());
        assert_eq!(output.unwrap().stdout, b"123");
    }
}
