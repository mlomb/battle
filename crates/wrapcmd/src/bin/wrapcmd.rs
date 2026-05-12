use clap::Parser;
use std::process::ExitCode;

use battle_wrapcmd::{wrapcmd_main, WrapCmdCli};

fn main() -> ExitCode {
    let cli = WrapCmdCli::parse();
    wrapcmd_main(cli)
}
