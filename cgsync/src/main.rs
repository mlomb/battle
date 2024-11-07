mod server;

use bundler::{bundle, BundlerArgs};
use clap::Parser;
use console::style;
use futures_channel::mpsc::channel;
use notify::{RecursiveMode, Watcher};
use server::start_server;
use std::collections::HashSet;

#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
struct Args {
    #[clap(flatten)]
    bundler_args: BundlerArgs,
}

#[tokio::main]
async fn main() {
    let args = Args::parse();

    let (mut code_tx, code_rx) = channel(100);

    start_server(code_rx).await;

    let (evt_tx, evt_rx) = std::sync::mpsc::channel();
    let mut watcher = notify::recommended_watcher(evt_tx).unwrap();
    let mut currently_watching = HashSet::new();

    loop {
        // bundle and send the code
        match bundle(&args.bundler_args) {
            Ok(bundle) => {
                code_tx.try_send(bundle.source).unwrap();
                println!("{} Code bundled and updated", style("U").green());

                for file in bundle.src_files {
                    if currently_watching.contains(&file) {
                        continue;
                    }

                    println!(
                        "{} Added file to watch: {}",
                        style("W").yellow(),
                        file.file_name().unwrap().to_str().unwrap()
                    );

                    watcher.watch(&file, RecursiveMode::NonRecursive).unwrap();
                    currently_watching.insert(file);
                }
            }
            Err(e) => {
                println!("{} Failed to bundle: {:?}", style("E").red(), e);
            }
        }

        // wait for a file change event
        let evt = evt_rx.recv().unwrap();

        println!(
            "{} Changes detected: {}",
            style("C").blue(),
            evt.unwrap()
                .paths
                .iter()
                .map(|f| f.file_name().unwrap().to_str().unwrap())
                .collect::<Vec<_>>()
                .join(", ")
        );
    }
}
