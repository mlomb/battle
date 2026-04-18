//! Textual transcript record/replay for stdin, stdout, and stderr proxies.

use std::path::PathBuf;

use clap::{Parser, Subcommand};

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
