use bundler::{Language, Source};
use console::style;
use log::info;
use std::{path::Path, process::Command};
use tempfile::tempdir;

use crate::exec::{CommandExt, Executable};

#[derive(Debug)]
pub enum BuildError {
    /// Build system missing
    MissingCompiler(String),
    /// Some I/O error occurred
    IoError(String),
    /// The compiler did not exit successfully
    CompilerErrored {
        exit_code: Option<i32>,
        stdout: String,
        stderr: String,
    },
}

pub trait BuildExecutable {
    /// Compiles the source code for the current platform (runtime) and returns an [`Executable`]
    fn build(&self) -> Result<Executable, BuildError>;
}

impl BuildExecutable for Source {
    fn build(&self) -> Result<Executable, BuildError> {
        match self.language {
            Language::Cpp => build_cpp(&self.code),
            Language::Rust => build_rust(&self.code),
        }
    }
}

fn build_cpp(src: &String) -> Result<Executable, BuildError> {
    let dir = tempfile::tempdir().expect("failed to create temp dir");
    let src_path = dir.path().join("main.cpp");
    let out_path = if cfg!(windows) {
        dir.path().join("main.exe")
    } else {
        dir.path().join("main")
    };

    std::fs::write(&src_path, src).expect("failed to write source file");

    let target = format!("{}-{}", std::env::consts::ARCH, std::env::consts::OS);

    let mut cmd = Command::new("zig");
    cmd.args([
        "c++",
        "-O3",
        "-std=c++20",
        "-march=native",
        // if target is not specified, some symbols will fail to resolve in Apple Silicon
        "-target",
        &target,
        "-lc++",
        // match GCC's default constexpr steps
        "-fconstexpr-steps=33554432",
        "-o",
    ])
    .arg(&out_path)
    .arg(&src_path);

    execute_build_command(
        &mut cmd,
        &out_path,
        "zig executable was not found, make sure it is in your PATH. Zig is used for C++ compilation, please install it from https://ziglang.org/learn/getting-started/#managers",
    )
}

fn build_rust(src: &String) -> Result<Executable, BuildError> {
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
        .arg("--manifest-path")
        .arg(&toml_path);

    execute_build_command(
        &mut cmd,
        &target_path,
        "cargo executable was not found, make sure it is in your PATH. Cargo is used for Rust compilation, please install it from https://www.rust-lang.org/tools/install",
    )
}

fn execute_build_command(
    build_command: &mut Command,
    target_binary: &Path,
    not_found_msg: &str,
) -> Result<Executable, BuildError> {
    info!(
        "Build command: {}",
        style(build_command.command_line_string()).cyan()
    );

    let output = build_command.output().map_err(|e| match e.kind() {
        std::io::ErrorKind::NotFound => BuildError::MissingCompiler(not_found_msg.to_string()),
        _ => BuildError::IoError(format!(
            "failed to invoke {:?}: {}",
            build_command.get_program(),
            e
        )),
    })?;

    if !output.status.success() {
        return Err(BuildError::CompilerErrored {
            exit_code: output.status.code(),
            stdout: String::from_utf8_lossy(&output.stdout).into_owned(),
            stderr: String::from_utf8_lossy(&output.stderr).into_owned(),
        });
    }

    Ok(Executable::from_binary(target_binary.to_path_buf())?)
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
chrono = "0.4.41"
itertools = "0.14.0"
libc = "0.2.175"
rand = "0.9.2"
regex = "1.11.2"
time = "0.3.43"
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
            BuildError::IoError(e) => write!(f, "IO error: {}", e),
        }
    }
}

impl From<std::io::Error> for BuildError {
    fn from(e: std::io::Error) -> Self {
        BuildError::IoError(format!("{:?}", e))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use bundler::{Language, Source};

    fn zig_assert() {
        assert!(
            Command::new("zig")
                .arg("version")
                .output()
                .map(|o| o.status.success())
                .unwrap_or(false),
            "zig not found; install from https://ziglang.org or run tests with --ignored when zig is unavailable"
        );
    }

    fn assert_compiler_errored(err: &BuildError) {
        let BuildError::CompilerErrored {
            exit_code,
            stdout,
            stderr,
        } = err
        else {
            panic!("expected CompilerErrored, got {:?}", err);
        };

        assert!(exit_code.is_some_and(|c| c != 0));
        assert!(
            !stdout.is_empty() || !stderr.is_empty(),
            "error must have feedback"
        );
        assert!(format!("{err}").contains("Compilation failed"));
    }

    #[test]
    fn build_cpp_minimal_succeeds() {
        zig_assert();

        Source {
            code: "int main() { return 0; }\n".into(),
            language: Language::Cpp,
        }
        .build()
        .expect("build should succeed");
    }

    #[test]
    fn build_rust_minimal_succeeds() {
        Source {
            code: "fn main() { println!(\"Hello, world!\"); }\n".into(),
            language: Language::Rust,
        }
        .build()
        .expect("build should succeed");
    }

    #[test]
    fn build_cpp_invalid_fails() {
        zig_assert();

        let err = Source {
            code: "not valid c++!!!\n".into(),
            language: Language::Cpp,
        }
        .build()
        .expect_err("should not compile");

        assert_compiler_errored(&err);
    }

    #[test]
    fn build_rust_invalid_fails() {
        let err = Source {
            code: "not valid rust!!!\n".into(),
            language: Language::Rust,
        }
        .build()
        .expect_err("should not compile");

        assert_compiler_errored(&err);
    }

    #[test]
    fn missing_compiler_maps_to_error() {
        let mut cmd = Command::new("nonexistent_compiler");
        let err = execute_build_command(
            &mut cmd,
            Path::new("/tmp/ignored"),
            "nonexistent_compiler not in PATH",
        )
        .expect_err("spawn should fail with NotFound");

        assert!(format!("{err}").contains("nonexistent_compiler not in PATH"));
        assert!(
            matches!(err, BuildError::MissingCompiler(msg) if msg.contains("nonexistent_compiler not in PATH"))
        );
    }

    #[test]
    #[cfg(unix)]
    fn non_executable_maps_to_io_error() {
        let mut cmd = Command::new("/dev/null");
        let err = execute_build_command(&mut cmd, Path::new("/tmp/ignored"), "unused")
            .expect_err("executing /dev/null should fail at spawn");

        assert!(format!("{err}").contains("failed to invoke"));
        assert!(matches!(err, BuildError::IoError(msg) if msg.contains("/dev/null")));
    }
}
