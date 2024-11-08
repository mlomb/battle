use bundler::{bundle, Bundle, BundlerArgs};
use console::style;
use notify::{Event, RecommendedWatcher, RecursiveMode, Watcher};
use std::{collections::HashSet, error::Error, path::PathBuf, sync::mpsc::Receiver, thread};

type CodeTx = tokio::sync::watch::Sender<String>;

pub struct ProjectWatcher {
    bundler_args: BundlerArgs,

    file_watcher: RecommendedWatcher,
    event_rx: Receiver<notify::Result<Event>>,
    currently_watching: HashSet<PathBuf>,
}

impl ProjectWatcher {
    pub fn new(bundler_args: BundlerArgs) -> Self {
        let (event_tx, event_rx) = std::sync::mpsc::channel();
        let watcher = notify::recommended_watcher(event_tx).unwrap();

        Self {
            bundler_args,
            event_rx,
            file_watcher: watcher,
            currently_watching: Default::default(),
        }
    }

    fn run_loop(mut self, code_tx: CodeTx) {
        loop {
            // bundle the project
            match self.run_bundler() {
                Ok(bundle) => code_tx.send(bundle.source).unwrap(),
                Err(err) => println!("{} Failed to bundle: {:?}", style("[E]").red(), err),
            }

            // wait for changes
            self.wait_for_changes();
        }
    }

    fn run_bundler(&mut self) -> Result<Bundle, Box<dyn Error>> {
        let bundle = bundle(&self.bundler_args)?;

        // add new files to watch
        for file in &bundle.src_files {
            if self.currently_watching.contains(file) {
                continue;
            }

            println!(
                "{} Added file to watch: {}",
                style("[W]").yellow(),
                style(file.file_name().unwrap().to_str().unwrap()).magenta()
            );

            self.file_watcher
                .watch(&file, RecursiveMode::NonRecursive)
                .unwrap();
            self.currently_watching.insert(file.to_path_buf());
        }

        Ok(bundle)
    }

    fn wait_for_changes(&mut self) {
        // block until a file changes
        let evt = self.event_rx.recv().unwrap();

        // sleep some ms to throttle the watcher
        // this allows the IDE to run any formatters
        std::thread::sleep(std::time::Duration::from_millis(100));

        // consume any other events so that they don't re-trigger
        while let Ok(_) = self.event_rx.try_recv() {}

        println!(
            "{} Changes detected: {}",
            style("[C]").blue(),
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

pub fn start_project_watcher(bundler_args: BundlerArgs, code_tx: CodeTx) {
    thread::spawn(|| ProjectWatcher::new(bundler_args).run_loop(code_tx));
}
