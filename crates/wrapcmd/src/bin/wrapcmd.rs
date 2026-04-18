use clap::Parser;
use std::process::ExitCode;
use wrapcmd::{capture, replay, transcript::Transcript, WrapCmdArgs, WrapCmdCommand};

fn main() -> ExitCode {
    let cli = WrapCmdArgs::parse();

    match cli.command {
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
