use bundler::source::{Language, Source};
use log::info;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::{
    fs::OpenOptions, io::Write, os::unix::fs::OpenOptionsExt, path::PathBuf, process::Command,
};
use tempfile::{TempDir, tempdir};

use crate::exec::executable::Executable;

#[derive(Debug)]
pub enum BuildError {
    /// Build system missing
    MissingCompiler(String),
    /// The compiler did not exit successfully
    CompilerErrored {
        exit_code: Option<i32>,
        stdout: String,
        stderr: String,
    },
    /// There was a problem constructing the [`ExecutableCommand`].
    /// This is a string to avoid recursion
    CommandError(String),
    /// Some I/O error occurred
    IoError(String),
}

pub trait SourceBuilder {
    /// Compiles the source code for the current platform (where the code is running) and returns an [`ExecutableCommand`]
    fn build(&self, files: HashMap<PathBuf, Vec<u8>>) -> Result<Executable, BuildError>;
}

impl SourceBuilder for Source {
    fn build(&self, files: HashMap<PathBuf, Vec<u8>>) -> Result<Executable, BuildError> {
        match self.language {
            Language::Cpp => build_cpp(&self.code, HashMap::new()),
            //Language::Rust => build_rust(&self.code, assets),
            Language::Rust => todo!(),
        }
    }
}

pub fn build_cpp(
    src: &String,
    mut files: HashMap<PathBuf, Vec<u8>>,
) -> Result<Executable, BuildError> {
    let dir = tempfile::tempdir().expect("failed to create temp dir");
    let src_path = dir.path().join("main.cpp");
    let out_path = dir.path().join("main");

    std::fs::write(&src_path, src).expect("failed to write source file");

    let target = format!("{}-{}", std::env::consts::ARCH, std::env::consts::OS);

    // NOTE: if target is not specified, some symbols will fail to resolve in Apple Silicon
    let output = Command::new("zig")
        .args([
            "c++",
            "-O3",
            "-std=c++20",
            "-march=native",
            "-target",
            &target,
            "-lc++",
            // match GCC's default constexpr steps
            "-fconstexpr-steps=33554432",
            "-o",
        ])
        .arg(&out_path)
        .arg(&src_path)
        .output();

    let output = output.map_err(|e| match e.kind() {
        std::io::ErrorKind::NotFound => BuildError::MissingCompiler(
            "zig executable was not found, make sure it is in your PATH. Zig is used for C++ cross-compilation, please install it from https://ziglang.org/learn/getting-started/#managers".to_string(),
        ),
        _ => BuildError::IoError(format!("failed to invoke zig c++: {}", e)),
    })?;

    if !output.status.success() {
        return Err(BuildError::CompilerErrored {
            exit_code: output.status.code(),
            stdout: String::from_utf8_lossy(&output.stdout).into_owned(),
            stderr: String::from_utf8_lossy(&output.stderr).into_owned(),
        });
    }

    Ok(Executable::from_binary(out_path)?)
}

fn build_rust(src: &String, assets: HashMap<String, Vec<u8>>) -> Result<(), BuildError> {
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

    execute_build_command(cmd, target_path, assets)
}

fn execute_build_command(
    mut build_command: Command,
    target_binary: PathBuf,
    assets: HashMap<String, Vec<u8>>,
) -> Result<(), BuildError> {
    // info!(
    //     "Build command: {}",
    //     style(build_command.command_line_string()).cyan()
    // );

    let output = build_command.output()?;

    /*
    if output.status.success() {
        return ExecutableCommand::from_binary(target_binary, assets)
            .map_err(|e| BuildError::CommandError(format!("{:?}", e)));
    } else {
        return Err(BuildError::CompilerErrored {
            exit_code: output.status.code(),
            stdout: String::from_utf8(output.stdout).unwrap(),
            stderr: String::from_utf8(output.stderr).unwrap(),
        });
    } */

    Ok(())
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

impl std::fmt::Display for BuildError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            BuildError::MissingCompiler(e) => write!(f, "Missing compiler: {}", e),
            BuildError::CompilerErrored {
                exit_code,
                stdout,
                stderr,
            } => {
                write!(f, "Compilation failed")?;
                if let Some(code) = exit_code {
                    write!(f, " (exit code {})", code)?;
                }
                if !stderr.is_empty() {
                    write!(f, "\n{}", stderr)?;
                }
                if !stdout.is_empty() {
                    write!(f, "\n{}", stdout)?;
                }
                Ok(())
            }
            BuildError::CommandError(e) => write!(f, "Command error: {}", e),
            BuildError::IoError(e) => write!(f, "IO error: {}", e),
        }
    }
}

impl From<std::io::Error> for BuildError {
    fn from(e: std::io::Error) -> Self {
        BuildError::IoError(format!("{:?}", e))
    }
}
