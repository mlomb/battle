use log::info;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
#[cfg(unix)]
use std::os::unix::fs::OpenOptionsExt;
use std::{fs::OpenOptions, io::Write, path::PathBuf, process::Command};
use tempfile::TempDir;
use wrapcmd::Transcript;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum ExecutableKind {
    /// A binary executable (.exe or ./main)
    Binary,
    /// A Java executable file (.jar)
    Jar,
    /// Python source code (.py)
    Python,
    /// A wrapcmd transcript to play back via `<current_exe> wrap playback <file>`.
    Playback,
}

#[derive(Serialize, Deserialize)]
pub struct Executable {
    /// The kind of executable
    kind: ExecutableKind,

    /// Entrypoint of the executable
    ///
    /// For example:
    ///   - `"main.py"` (Python)
    ///   - `"main"` (binary)
    ///   - `"main.jar"` (Java)
    entrypoint: PathBuf,

    /// Required files for the execution (like the binary itself).
    /// Can also include additional assets.
    ///
    /// For example:
    ///   - `{"main.py": b"print('Hello, World!')"}`
    ///   - `{"main": b"\x7fELF..."}` (binary)
    ///   - `{"main.jar": b"PK..."}` (jar)
    files: HashMap<PathBuf, Vec<u8>>,

    /// Current directory where files are located in disk.
    /// It is lazily initialized when `command` is called.
    /// It is reused across executions.
    /// Upon drop, the directory is removed.
    #[serde(skip)]
    tmp_workdir: Option<TempDir>,
}

impl Clone for Executable {
    fn clone(&self) -> Self {
        Self {
            kind: self.kind.clone(),
            entrypoint: self.entrypoint.clone(),
            files: self.files.clone(),
            tmp_workdir: None,
        }
    }
}

impl std::fmt::Debug for Executable {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let mut s = f.debug_struct("Executable");
        s.field("kind", &self.kind);
        let file_sizes: HashMap<&PathBuf, usize> =
            self.files.iter().map(|(k, v)| (k, v.len())).collect();
        s.field("files", &file_sizes);
        s.finish()
    }
}

impl Executable {
    /// Creates a new executable that runs a Jar file ("java -jar main.jar")
    pub fn from_jar(jar_path: PathBuf) -> Result<Self, std::io::Error> {
        let name = PathBuf::from(
            jar_path
                .file_name()
                .map(|x| x.to_string_lossy().to_string())
                .unwrap_or("jarfile.jar".to_string()),
        );

        Ok(Self {
            kind: ExecutableKind::Jar,
            entrypoint: name.clone(),
            files: HashMap::from([(name, std::fs::read(&jar_path)?)]),
            tmp_workdir: None,
        })
    }

    /// Creates a new executable that plays back a wrapcmd transcript via `<current_exe> wrap playback`.
    pub fn from_transcript(transcript: &Transcript) -> Self {
        let content = transcript.to_string().into_bytes();
        let name = PathBuf::from("transcript.io");
        Self {
            kind: ExecutableKind::Playback,
            entrypoint: name.clone(),
            files: HashMap::from([(name, content)]),
            tmp_workdir: None,
        }
    }

    /// Creates a new executable from a binary file (.exe or ./main)
    pub fn from_binary(binary_path: PathBuf) -> Result<Self, std::io::Error> {
        let name = PathBuf::from(
            binary_path
                .file_name()
                .map(|x| x.to_string_lossy().to_string())
                .unwrap_or("main".to_string()),
        );

        Ok(Self {
            kind: ExecutableKind::Binary,
            entrypoint: name.clone(),
            files: HashMap::from([(name, std::fs::read(&binary_path)?)]),
            tmp_workdir: None,
        })
    }

    /// Initializes the temporal directory and writes the files to it
    fn initialize_files(&mut self) -> Result<PathBuf, String> {
        if self.tmp_workdir.is_none() {
            info!(
                "Initializing executable {:?}, files: {:?} ({} bytes)",
                self.kind,
                self.files.keys(),
                self.files.values().map(|v| v.len()).sum::<usize>()
            );

            let tmp =
                TempDir::new().map_err(|io_err| format!("failed to create temp dir: {io_err}"))?;

            // write files to disk
            for (name, content) in &self.files {
                let path = tmp.path().join(name);
                let mut opts = OpenOptions::new();
                opts.create(true).write(true);
                #[cfg(unix)]
                opts.mode(0o755);
                let mut file = opts
                    .open(&path)
                    .map_err(|io_err| format!("failed to open file: {path:?}: {io_err}"))?;

                file.write_all(content)
                    .map_err(|io_err| format!("failed to write file: {path:?}: {io_err}"))?;
            }

            self.tmp_workdir = Some(tmp);
        }

        Ok(self
            .tmp_workdir
            .as_ref()
            .expect("initialized")
            .path()
            .to_owned())
    }

    pub fn command(&mut self) -> Command {
        let dir = self.initialize_files().expect("failed to initialize files");
        let entry = dir.join(self.entrypoint.clone());

        match &self.kind {
            ExecutableKind::Binary => Command::new(entry),
            ExecutableKind::Jar => {
                let mut cmd = Command::new("java");
                // This is due CodinGame engine accessing internal classes which is not supported in modern Java
                cmd.args(["--add-opens", "java.base/java.lang=ALL-UNNAMED", "-jar"])
                    .arg(entry);
                cmd
            }
            ExecutableKind::Playback => {
                let current_exe =
                    std::env::current_exe().expect("failed to get current executable path");
                let mut cmd = Command::new(current_exe);
                cmd.args(["wrap", "playback"]).arg(entry);
                cmd
            }
            _ => todo!(),
        }
    }
}
