use super::executable_command::{ExecutableCommand, ExecutableCommandError};
use bundler::source::{Language, Source};
use current_platform::CURRENT_PLATFORM;
use std::{path::PathBuf, process::Command};
use tempfile::tempdir;

pub enum BuildError {
    /// Some IO error occurred
    Io(std::io::Error),
    /// The compiler did not exit successfully
    CompilerErrored {
        exit_code: Option<i32>,
        stdout: String,
        stderr: String,
    },
    /// There was a problem constructing the [`ExecutableCommand`]
    CommandError(ExecutableCommandError),
}

pub trait SourceBuilder {
    /// Compiles the source code for the current platform (where the code is running) and returns an [`ExecutableCommand`]
    fn build(&self) -> Result<ExecutableCommand, BuildError>;
}

impl SourceBuilder for Source {
    fn build(&self) -> Result<ExecutableCommand, BuildError> {
        match self.language {
            Language::Cpp => build_cpp(&self.code),
            Language::Rust => build_rust(&self.code),
        }
    }
}

fn build_cpp(src: &String) -> Result<ExecutableCommand, BuildError> {
    let temp_dir = tempdir()?;
    let src_path = temp_dir.path().join("main.cpp");
    let exe_path = temp_dir.path().join("main.exe");
    let obj_path = temp_dir.path().join("main.obj");

    std::fs::write(&src_path, src)?;

    let mut build = cc::Build::new();
    build
        .cpp(true)
        .std("c++20")
        .opt_level(3)
        .debug(false)
        .target(CURRENT_PLATFORM)
        .host(CURRENT_PLATFORM)
        .out_dir(temp_dir)
        .warnings(false)
        .cargo_metadata(false); // silence

    let mut cmd = build.get_compiler().to_command();
    cmd.arg("/W0");
    cmd.arg(format!("/Fe{}", exe_path.display().to_string()));
    cmd.arg(format!("/Fo{}", obj_path.display().to_string()));
    cmd.arg(src_path);

    execute_build_command(cmd, exe_path)
}

fn build_rust(src: &String) -> Result<ExecutableCommand, BuildError> {
    let temp_dir = tempdir()?;
    let toml_path = temp_dir.path().join("Cargo.toml");
    let src_path = temp_dir.path().join("main.rs");
    let target_path = temp_dir.path().join(if cfg!(windows) {
        "target/release/main.exe"
    } else {
        "target/release/main"
    });

    std::fs::write(&toml_path, CARGO_TOML)?;
    std::fs::write(&src_path, src)?;

    let mut cmd = Command::new("cargo");
    cmd.arg("build")
        .arg("--release")
        .current_dir(temp_dir.path());

    execute_build_command(cmd, target_path)
}

fn execute_build_command(
    mut build_command: Command,
    target_binary: PathBuf,
) -> Result<ExecutableCommand, BuildError> {
    let output = build_command.output()?;

    if output.status.success() {
        return ExecutableCommand::from_binary(target_binary).map_err(BuildError::CommandError);
    } else {
        return Err(BuildError::CompilerErrored {
            exit_code: output.status.code(),
            stdout: String::from_utf8(output.stdout).unwrap(),
            stderr: String::from_utf8(output.stderr).unwrap(),
        });
    }
}

/// Cargo configuration for compiling Rust code.
///
/// Dependencies are taken from CodinGame's available libraries
/// https://www.codingame.com/playgrounds/40701/help-center/languages-versions
const CARGO_TOML: &str = r#"
[package]
name = "main"
version = "0.1.0"
edition = "2021"

[profile.release]
lto = true

[[bin]]
name = "main"
path = "main.rs"

[dependencies]
chrono = "0.4.26"
itertools = "0.11.0"
libc = "0.2.147"
rand = "0.8.5"
regex = "1.8.4"
time = "0.3.22"
"#;

impl From<std::io::Error> for BuildError {
    fn from(e: std::io::Error) -> Self {
        BuildError::Io(e)
    }
}
