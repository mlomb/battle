use battle_wrapcmd::Transcript;
use log::info;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
#[cfg(unix)]
use std::os::unix::fs::OpenOptionsExt;
use std::{fs::OpenOptions, io::Write, path::PathBuf, process::Command};
use tempfile::TempDir;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum ExecutableKind {
    /// A binary executable (.exe or ./main)
    Binary,
    /// A Java executable file (.jar)
    Jar,
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
        }
    }
}

#[cfg(test)]
mod tests {
    use std::{fs::File, path::Path};

    use battle_wrapcmd::Event;
    use tempfile::TempDir;

    use crate::exec::BuildExecutable;
    use battle_bundler::{Language, Source};

    use super::*;

    fn sample_binary() -> Executable {
        Source {
            code: "int main() { return 0; }\n".into(),
            language: Language::Cpp,
        }
        .build()
        .expect("sample binary should build")
    }

    const SAMPLE_JAR_BYTES: &[u8] = &[
        0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    ];

    fn sample_jar() -> Executable {
        let dir = TempDir::new().expect("temp dir for jar");
        let path = dir.path().join("sample.jar");
        std::fs::write(&path, SAMPLE_JAR_BYTES).expect("write minimal jar");
        Executable::from_jar(path).expect("jar executable to be created")
    }

    fn sample_transcript() -> Executable {
        let transcript = Transcript {
            events: vec![
                Event::In("hello".into()),
                Event::Out("world".into()),
                Event::Err("error".into()),
            ],
        };
        Executable::from_transcript(&transcript)
    }

    #[test]
    fn command_program_should_exist() {
        let mut exe = sample_binary();
        let cmd = exe.command();

        assert!(
            Path::new(cmd.get_program()).is_file(),
            "command program should exist on disk: {:?}",
            cmd.get_program()
        );
        assert_eq!(cmd.get_args().len(), 0, "binary should have no arguments");
    }

    #[test]
    fn command_must_reuse_files() {
        let mut exe = sample_binary();
        let cmd1 = exe.command();
        let cmd2 = exe.command();
        let mut exe_cloned = exe.clone();
        let cmd3 = exe_cloned.command();
        let cmd4 = exe_cloned.command();
        assert_eq!(cmd1.get_program(), cmd2.get_program());
        assert_eq!(cmd3.get_program(), cmd4.get_program());
        // cloned should be different
        assert_ne!(cmd1.get_program(), cmd3.get_program());
    }

    #[test]
    fn debug_does_not_print_full_binary_content() {
        let mut exe = sample_binary();

        assert!(
            File::open(exe.command().get_program())
                .expect("program to exist")
                .metadata()
                .expect("metadata to be retrieved")
                .len()
                > 1000,
            "program should be larger than 1000 bytes"
        );

        assert!(
            format!("{:?}", exe).len() < 1000,
            "debug should not print full binary content"
        );
    }

    #[test]
    fn jar_command_should_invoke_java_and_our_jar() {
        let mut exe = sample_jar();
        let cmd = exe.command();
        assert_eq!(cmd.get_program(), "java");

        let args: Vec<_> = cmd.get_args().collect();
        let jar_idx = args
            .iter()
            .position(|a| *a == std::ffi::OsStr::new("-jar"))
            .expect("java argv should contain -jar");
        let jar_os = args
            .get(jar_idx + 1)
            .expect("java argv should have jar path after -jar");

        assert_eq!(
            std::fs::read(jar_os).expect("read jar from workdir"),
            SAMPLE_JAR_BYTES,
            "jar on disk should match bytes loaded into Executable"
        );
    }

    #[test]
    fn playback_transcript_is_on_disk() {
        let mut exe = sample_transcript();
        let cmd = exe.command();
        let transcript_path = cmd
            .get_args()
            .find(|a| Path::new(a).extension() == Some(std::ffi::OsStr::new("io")))
            .expect("argv should include a path ending in .io");
        assert!(
            !std::fs::read(transcript_path)
                .expect("read transcript")
                .is_empty()
        );
    }
}
