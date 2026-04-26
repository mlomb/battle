use clap::Parser;
use std::process::ExitCode;

use wrapcmd::{wrapcmd_main, WrapCmdCli};

fn main() -> ExitCode {
    let cli = WrapCmdCli::parse();
    wrapcmd_main(cli)
}
