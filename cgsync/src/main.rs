mod server;

use bundler::{bundle, BundlerArgs};
use clap::Parser;
use console::style;
use notify::{RecursiveMode, Watcher};
use server::start_ws_server;
use std::collections::HashSet;
use tokio::sync::watch;

#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
struct Args {
    #[clap(flatten)]
    bundler_args: BundlerArgs,
}

#[tokio::main]
async fn main() {
    let args = Args::parse();

    let (code_tx, code_rx) = watch::channel("".to_string());

    start_ws_server(code_rx).await;

    let (evt_tx, evt_rx) = std::sync::mpsc::channel();
    let mut watcher = notify::recommended_watcher(evt_tx).unwrap();
    let mut currently_watching = HashSet::new();

    loop {
        // bundle and send the code
        match bundle(&args.bundler_args) {
            Ok(bundle) => {
                // add new files to watch
                for file in bundle.src_files {
                    if currently_watching.contains(&file) {
                        continue;
                    }

                    println!(
                        "{} Added file to watch: {}",
                        style("W").yellow(),
                        style(file.file_name().unwrap().to_str().unwrap()).magenta()
                    );

                    watcher.watch(&file, RecursiveMode::NonRecursive).unwrap();
                    currently_watching.insert(file);
                }

                code_tx
                    .send(bundle.source)
                    .expect("at least one receiver (_rx)");

                println!("{} Code updated", style("U").green());
            }
            Err(e) => {
                println!("{} Failed to bundle: {:?}", style("E").red(), e);
            }
        }

        // wait for a file change event
        let evt = evt_rx.recv().unwrap();

        // sleep some ms to throttle the watcher
        // this allows the IDE to run any formatters
        std::thread::sleep(std::time::Duration::from_millis(100));

        // consume any other events so that they don't re-trigger
        while let Ok(_) = evt_rx.try_recv() {}

        println!(
            "{} Changes detected: {}",
            style("C").blue(),
            style(
                evt.unwrap()
                    .paths
                    .iter()
                    .map(|f| f.file_name().unwrap().to_str().unwrap())
                    .collect::<Vec<_>>()
                    .join(", ")
            )
            .magenta()
        );
    }
}
