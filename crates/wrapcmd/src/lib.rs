use clap::{Parser, Subcommand};
use std::{path::PathBuf, process::ExitCode};

use crate::transcript::Transcript;

pub mod capture;
pub mod replay;
pub mod transcript;

#[derive(Parser, Debug)]
pub struct WrapCmdArgs {
    #[command(subcommand)]
    pub command: WrapCmdCommand,
}

#[derive(Subcommand, Debug, Clone)]
pub enum WrapCmdCommand {
    /// Proxies and captures stdin/stdout/stderr of a command to a transcript file.
    ///
    /// From the invoker's perspective, the behaviour of the command is the same as if it was run directly.
    Capture {
        /// Transcript file to write.
        #[arg(value_name = "OUT")]
        out: PathBuf,

        /// Command and arguments (use `--` before the command if it starts with `-`).
        #[arg(trailing_var_arg = true, allow_hyphen_values = true, required = true)]
        cmd: Vec<std::ffi::OsString>,
    },

    /// Reads a transcript and replays it.
    ///
    /// It will consume stdin, checking that stdin matches the transcript.
    /// At the same time, it will write to stdout and stderr in the same order.
    Replay {
        /// Path to the transcript file.
        transcript: PathBuf,
    },
}

pub fn wrap_main(command: WrapCmdCommand) -> ExitCode {
    match command {
        WrapCmdCommand::Capture { out, cmd } => {
            let (transcript, code) = capture::run_capture(&cmd);
            if let Err(e) = transcript.save(&out) {
                eprintln!("wrapcmd capture: save {}: {e}", out.display());
                return ExitCode::FAILURE;
            }
            code
        }
        WrapCmdCommand::Replay { transcript: path } => {
            let text = std::fs::read_to_string(&path).expect("read transcript");
            let transcript: Transcript = text.parse().expect("parse transcript");
            replay::run_replay(&transcript)
        }
    }
}
