use std::process::Command;
use std::time::Duration;

use assert_cmd::cargo::CommandCargoExt;
use send_ctrlc::{Interruptible, InterruptibleChild, InterruptibleCommand};

pub struct BattleWorker {
    child: InterruptibleChild,
}

impl BattleWorker {
    pub fn spawn() -> Self {
        let child = Command::cargo_bin("battle")
            .expect("cargo_bin battle")
            .args(["worker", "--threads", "1"])
            .spawn_interruptible()
            .expect("spawn battle worker");

        Self { child }
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
