use battle_bundler::{BundlerArgs, bundle};
use console::{Emoji, style};
use log::info;

use crate::exec::{BuildError, BuildExecutable, Executable};

static BUILDING: Emoji<'_, '_> = Emoji("🏗️ ", "");
static BOX: Emoji<'_, '_> = Emoji("📦 ", "");

pub fn bundle_and_build(bundler_args: BundlerArgs) -> Result<Executable, BuildError> {
    info!(
        "{} {}Bundling project... {}",
        style("[1/2]").bold().dim(),
        BOX,
        bundler_args.entry.clone().unwrap().to_string_lossy()
    );

    let bundle = bundle(&bundler_args).expect("correct bundle");

    info!("  OK {} bytes", bundle.source.code.len());

    info!(
        "{} {}Building binary...",
        style("[2/2]").bold().dim(),
        BUILDING
    );

    bundle.source.build()
}

pub fn build_main(bundler_args: BundlerArgs) {
    match bundle_and_build(bundler_args) {
        Ok(exec) => println!("  OK: {:?}", exec),
        Err(BuildError::MissingCompiler(e)) => {
            eprintln!("Missing compiler: {}", e);
            std::process::exit(1);
        }
        Err(e) => {
            eprintln!("Error: {:?}", e);
            std::process::exit(1);
        }
    }
}
