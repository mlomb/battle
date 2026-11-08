use std::process::Command;
use std::sync::{Mutex, MutexGuard};
use std::time::Duration;

use assert_cmd::cargo::CommandCargoExt;
use send_ctrlc::{Interruptible, InterruptibleChild, InterruptibleCommand};

static WORKER_PORT: Mutex<()> = Mutex::new(());

/// Spawns a battle worker and waits for it to be ready.
/// Used for tests.
pub struct BattleWorker {
    child: InterruptibleChild,
    _port: MutexGuard<'static, ()>,
}

impl BattleWorker {
    pub fn spawn() -> Self {
        let port = WORKER_PORT.lock().unwrap_or_else(|e| e.into_inner());

        let child = Command::cargo_bin("battle")
            .expect("cargo_bin battle")
            .args(["worker", "--threads", "1"])
            .spawn_interruptible()
            .expect("spawn battle worker");

        // wait for server to be ready (we could use TCP but meh)
        std::thread::sleep(Duration::from_millis(500));

        Self { child, _port: port }
    }
}

impl Drop for BattleWorker {
    fn drop(&mut self) {
        // give a chance to the child to exit gracefully
        // we want this to have proper line coverage, otherwise it is lost
        self.child.interrupt().expect("interrupt child");

        std::thread::sleep(Duration::from_millis(500));

        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}
