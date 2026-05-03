use std::time::Duration;

use send_ctrlc::{Interruptible, InterruptibleChild};

pub struct ExecGuard(pub InterruptibleChild);

impl Drop for ExecGuard {
    fn drop(&mut self) {
        println!("dropping exec guard");

        self.0.interrupt().expect("interrupt child");

        // give a chance to the child to exit gracefully
        std::thread::sleep(Duration::from_millis(500));

        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}
