use clap::{Parser, Subcommand};
use std::{path::PathBuf, process::ExitCode};

mod capture;
mod playback;
mod transcript;

pub use transcript::{Event, Transcript};

/// Wraps an executable to record/playback all I/O streams
#[derive(Parser, Debug)]
pub struct WrapCmdCli {
    #[command(subcommand)]
    command: WrapCmdCommand,
}

#[derive(Subcommand, Debug, Clone)]
enum WrapCmdCommand {
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

    /// Reads a transcript file and plays it back.
    ///
    /// It will consume stdin, checking that stdin matches the transcript.
    /// At the same time, it will write to stdout and stderr in the same order.
    Playback {
        /// Path to the transcript file.
        transcript: PathBuf,
    },
}

pub fn wrapcmd_main(cli: WrapCmdCli) -> ExitCode {
    match cli.command {
        WrapCmdCommand::Capture { out, cmd } => capture::run_capture(&cmd, &out),
        WrapCmdCommand::Playback { transcript: path } => {
            let text = std::fs::read_to_string(&path).expect("read transcript");
            let transcript: Transcript = text.parse().expect("parse transcript");
            playback::run_playback(&transcript)
        }
    }
}
