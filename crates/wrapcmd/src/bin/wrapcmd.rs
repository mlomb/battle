use clap::Parser;
use std::process::ExitCode;

use wrapcmd::{wrap_main, WrapCmdArgs};

fn main() -> ExitCode {
    let cli = WrapCmdArgs::parse();

    wrap_main(cli.command)
}
