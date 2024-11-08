use std::process::Command;

use bundler::BundleLanguage;
use current_platform::CURRENT_PLATFORM;
use tempfile::tempdir;

/// Takes a source code string and returns the compiled binary
pub fn build_source(
    src: &String,
    lang: BundleLanguage,
) -> Result<Vec<u8>, Box<dyn std::error::Error>> {
    match lang {
        BundleLanguage::Cpp => build_cpp(src),
        BundleLanguage::Rust => build_rust(src),
    }
}

fn build_cpp(src: &String) -> Result<Vec<u8>, Box<dyn std::error::Error>> {
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
        //.out_dir(temp_dir)
        .warnings(false)
        .cargo_metadata(false); // silence

    let mut cmd = build.get_compiler().to_command();
    cmd.arg("/W0");
    cmd.arg(format!("/Fe{}", exe_path.display().to_string()));
    cmd.arg(format!("/Fo{}", obj_path.display().to_string()));
    cmd.arg(src_path);

    println!("{:?}", cmd);
    let output = cmd.output().unwrap();

    if output.status.success() {
        // read binary file
        return Ok(std::fs::read(exe_path)?);
    } else {
        return Err(Box::new(std::io::Error::new(
            std::io::ErrorKind::Other,
            String::from_utf8(output.stderr).unwrap(),
        )));
    }
}

fn build_rust(src: &String) -> Result<Vec<u8>, Box<dyn std::error::Error>> {
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

    let output = Command::new("cargo")
        .arg("build")
        .arg("--release")
        .current_dir(temp_dir.path())
        .output()
        .unwrap();

    if output.status.success() {
        // read binary file
        return Ok(std::fs::read(target_path)?);
    } else {
        return Err(Box::new(std::io::Error::new(
            std::io::ErrorKind::Other,
            String::from_utf8(output.stderr).unwrap(),
        )));
    }
}

/// Cargo configuration for compiling Rust code
// Dependencies are taken from CodinGame's available libraries
// https://www.codingame.com/playgrounds/40701/help-center/languages-versions
const CARGO_TOML: &str = r#"
[package]
name = "main"
version = "0.1.0"
edition = "2021"

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
