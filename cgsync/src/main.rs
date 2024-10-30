mod server;

use bundler::bundle;
use clap::Parser;
use futures_channel::mpsc::channel;
use notify::{RecursiveMode, Watcher};
use server::start_server;
use std::path::PathBuf;

#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
struct Args {
    #[arg(default_value = ".")]
    project_path: PathBuf,
}

#[tokio::main]
async fn main() {
    let args = Args::parse();

    let (mut code_tx, code_rx) = channel(100);

    start_server(code_rx).await;

    let watch_path = args.project_path.clone();
    let (evt_tx, evt_rx) = std::sync::mpsc::channel();
    let mut watcher = notify::recommended_watcher(evt_tx).unwrap();
    watcher
        .watch(&watch_path, RecursiveMode::Recursive)
        .unwrap();
    watcher
        .watch(&watch_path.join("src"), RecursiveMode::Recursive)
        .unwrap();
    watcher.unwatch(&watch_path.join("target")).unwrap();

    println!("Watching \"{}\"", watch_path.display());

    loop {
        // bundle and send the code
        match bundle(&args.project_path) {
            Ok(bundle) => {
                code_tx.try_send(bundle.source).unwrap();
            }
            Err(e) => {
                println!("Failed to bundle: {:?}", e);
            }
        }

        // wait for a file change event
        let evt = evt_rx.recv().unwrap();

        println!(
            "Changes detected: {}",
            evt.unwrap()
                .paths
                .iter()
                .map(|f| f.file_name().unwrap().to_str().unwrap())
                .collect::<Vec<_>>()
                .join(", ")
        );
    }
}
