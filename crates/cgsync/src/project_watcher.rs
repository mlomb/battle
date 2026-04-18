use bundler::{bundle, Bundle, BundlerArgs};
use console::style;
use notify::{Event, RecommendedWatcher, RecursiveMode, Watcher};
use std::{collections::HashSet, error::Error, path::PathBuf, sync::mpsc::Receiver};

type CodeTx = tokio::sync::watch::Sender<String>;

/// Watches a project for changes and bundles it.
/// Sends the bundled code to a channel.
pub struct ProjectWatcher {
    /// The arguments to pass to the bundler
    bundler_args: BundlerArgs,

    /// The file watcher
    file_watcher: RecommendedWatcher,
    /// The receiver for file change events
    event_rx: Receiver<notify::Result<Event>>,
    /// The files that are currently being watched
    currently_watching: HashSet<PathBuf>,
}

impl ProjectWatcher {
    pub fn new(bundler_args: BundlerArgs) -> Self {
        let (event_tx, event_rx) = std::sync::mpsc::channel();
        let file_watcher = notify::recommended_watcher(event_tx).unwrap();

        Self {
            bundler_args,
            event_rx,
            file_watcher,
            currently_watching: Default::default(),
        }
    }

    fn run_loop(mut self, code_tx: CodeTx) {
        loop {
            // bundle the project
            match self.run_bundler() {
                Ok(bundle) => code_tx.send(bundle.source.code).expect("a code receiver"),
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
            let file = std::fs::canonicalize(file).unwrap();

            if self.currently_watching.contains(&file) {
                continue;
            }

            println!(
                "{} Added file to watch: {}",
                style("[W]").yellow(),
                style(file.file_name().unwrap().to_str().unwrap()).magenta()
            );

            self.file_watcher
                .watch(&file, RecursiveMode::NonRecursive)
                .expect("be able to watch a file");
            self.currently_watching.insert(file.to_path_buf());
        }

        Ok(bundle)
    }

    fn wait_for_changes(&mut self) {
        // block until a file changes
        let evt = self.event_rx.recv().expect("a file event");
        let mut changed_paths = evt.unwrap().paths;

        // sleep some ms to throttle the watcher
        // this allows the IDE to run any formatters
        std::thread::sleep(std::time::Duration::from_millis(100));

        // consume any other events so that they don't re-trigger
        while let Ok(Ok(evt)) = self.event_rx.try_recv() {
            changed_paths.extend(evt.paths);
        }

        println!(
            "{} Changes detected: {}",
            style("[C]").blue(),
            style(
                HashSet::<PathBuf>::from_iter(changed_paths.iter().cloned())
                    .iter()
                    .map(|f| std::fs::canonicalize(f).unwrap())
                    .map(|f| f
                        .clone()
                        .file_name()
                        .unwrap()
                        .to_string_lossy()
                        .into_owned())
                    .collect::<Vec<_>>()
                    .join(", ")
            )
            .magenta()
        );
    }
}

pub fn run_project_watcher(bundler_args: BundlerArgs, code_tx: CodeTx) {
    ProjectWatcher::new(bundler_args).run_loop(code_tx);
}
