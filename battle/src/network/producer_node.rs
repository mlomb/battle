use anyhow::{Result, bail};
use futures_util::StreamExt;
use log::{info, trace};
use serde::{Serialize, de::DeserializeOwned};
use std::net::SocketAddr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, RwLock};
use tokio::runtime::Runtime;
use tokio::sync::Notify;

use super::{
    ClientMsg, ServerMsg, TargetId, compute_target_id, random_peer_id, sanitize_protocol,
    start_discovery, ws_recv, ws_send,
};

struct Shared<T> {
    targets: RwLock<Vec<(TargetId, T)>>,
    target_count: AtomicUsize,
    target_notify: Notify,
}

pub struct ProducerHandle<T, W, R> {
    _runtime: Runtime,
    shared: Arc<Shared<T>>,
    work_tx: async_channel::Sender<W>,
    result_rx: Mutex<tokio::sync::mpsc::UnboundedReceiver<R>>,
    error_rx: Mutex<tokio::sync::mpsc::UnboundedReceiver<(TargetId, String)>>,
}

impl<T, W, R> ProducerHandle<T, W, R>
where
    T: Serialize + Clone + Send + Sync + 'static,
    W: Serialize + Clone + Send + Sync + 'static,
    R: DeserializeOwned + Send + 'static,
{
    pub fn new(protocol: &str) -> Self {
        let runtime = Runtime::new().expect("tokio runtime");
        let (work_tx, work_rx) = async_channel::bounded::<W>(1);
        let (result_tx, result_rx) = tokio::sync::mpsc::unbounded_channel::<R>();
        let (error_tx, error_rx) =
            tokio::sync::mpsc::unbounded_channel::<(TargetId, String)>();
        let shared = Arc::new(Shared {
            targets: RwLock::new(Vec::new()),
            target_count: AtomicUsize::new(0),
            target_notify: Notify::new(),
        });

        let service = sanitize_protocol(protocol);
        let peer_id = random_peer_id();
        let shared2 = shared.clone();
        let work_tx2 = work_tx.clone();

        runtime.spawn(async move {
            let handle = tokio::runtime::Handle::current();
            let (mut disc_rx, _guard) =
                start_discovery(service, peer_id, None, &handle).expect("mDNS");

            while let Some(addr) = disc_rx.recv().await {
                info!("Discovered worker at {}", addr);
                let shared = shared2.clone();
                let work_rx = work_rx.clone();
                let work_tx = work_tx2.clone();
                let result_tx = result_tx.clone();
                let error_tx = error_tx.clone();
                tokio::spawn(async move {
                    if let Err(e) =
                        producer_conn(addr, shared, work_rx, work_tx, result_tx, error_tx).await
                    {
                        info!("Worker {} disconnected: {}", addr, e);
                    }
                });
            }
        });

        ProducerHandle {
            _runtime: runtime,
            shared,
            work_tx,
            result_rx: Mutex::new(result_rx),
            error_rx: Mutex::new(error_rx),
        }
    }

    /// Register a target. Returns its content-hash [`TargetId`].
    /// Duplicates (same content) are deduplicated automatically.
    pub fn register_target(&self, target: T) -> TargetId {
        let id = compute_target_id(&target);
        let mut targets = self.shared.targets.write().unwrap();
        if targets.iter().any(|(h, _)| *h == id) {
            return id;
        }
        targets.push((id, target));
        self.shared
            .target_count
            .store(targets.len(), Ordering::Release);
        drop(targets);
        self.shared.target_notify.notify_waiters();
        id
    }

    pub fn send_work(&self, work: W) {
        self.work_tx.send_blocking(work).expect("producer active");
    }

    pub fn recv_result(&self) -> Option<R> {
        self.result_rx.lock().unwrap().blocking_recv()
    }

    pub fn recv_error(&self) -> Option<(TargetId, String)> {
        self.error_rx.lock().unwrap().blocking_recv()
    }
}

async fn producer_conn<T, W, R>(
    addr: SocketAddr,
    shared: Arc<Shared<T>>,
    work_rx: async_channel::Receiver<W>,
    work_tx: async_channel::Sender<W>,
    result_tx: tokio::sync::mpsc::UnboundedSender<R>,
    error_tx: tokio::sync::mpsc::UnboundedSender<(TargetId, String)>,
) -> Result<()>
where
    T: Serialize + Clone,
    W: Serialize + Clone + Send,
    R: DeserializeOwned,
{
    let tcp = tokio::net::TcpStream::connect(addr).await?;
    tcp.set_nodelay(true)?;
    let (ws, _) = tokio_tungstenite::client_async(format!("ws://{addr}"), tcp).await?;
    info!("Connected to worker {}", addr);
    let (mut sink, mut source) = ws.split();
    let mut sent: usize = 0;

    loop {
        while sent < shared.target_count.load(Ordering::Acquire) {
            let (id, target) = shared.targets.read().unwrap()[sent].clone();
            trace!("Sending target {:016x} to {}", id, addr);
            ws_send(&mut sink, &ServerMsg::<T, W>::Target(target)).await?;

            match ws_recv::<ClientMsg<R>, _>(&mut source).await? {
                ClientMsg::TargetOk(h) => info!("Target {:016x} OK from {}", h, addr),
                ClientMsg::TargetError { hash, error } => {
                    let _ = error_tx.send((hash, error.clone()));
                    bail!("Target {:016x} build failed on {}: {}", hash, addr, error);
                }
                ClientMsg::WorkResult(_) => bail!("expected target ACK, got WorkResult"),
            }
            sent += 1;
        }

        tokio::select! {
            biased;
            _ = shared.target_notify.notified() => continue,
            work = work_rx.recv() => {
                let work = work.map_err(|_| anyhow::anyhow!("work channel closed"))?;

                if sent < shared.target_count.load(Ordering::Acquire) {
                    let _ = work_tx.send(work).await;
                    continue;
                }

                let msg = ServerMsg::<T, W>::Work(work);
                if let Err(e) = ws_send(&mut sink, &msg).await {
                    if let ServerMsg::Work(w) = msg {
                        let _ = work_tx.send(w).await;
                    }
                    return Err(e);
                }
                trace!("Sent work to {}", addr);

                match ws_recv::<ClientMsg<R>, _>(&mut source).await? {
                    ClientMsg::WorkResult(r) => {
                        result_tx
                            .send(r)
                            .map_err(|_| anyhow::anyhow!("result channel closed"))?;
                    }
                    ClientMsg::TargetOk(_) | ClientMsg::TargetError { .. } => {
                        bail!("expected WorkResult, got setup response")
                    }
                }
            }
        }
    }
}
