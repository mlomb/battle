use futures::channel::mpsc;
use futures::channel::oneshot;
use futures::prelude::*;
use futures::StreamExt;
use libp2p::gossipsub::{MessageId, PublishError};
#[allow(deprecated)]
use libp2p::swarm::{SwarmEvent, THandlerErr};
use libp2p::Swarm;
use libp2p::{gossipsub, mdns, swarm::NetworkBehaviour};
use serde::de::DeserializeOwned;
use serde::Deserialize;
use serde::Serialize;
use std::error::Error;
use std::time::Duration;
use tokio::select;

#[derive(Debug, Serialize, Deserialize)]
pub enum Message<W, R> {
    /// A thread is available in this node, it needs work
    RequestWork,

    /// Work description
    ProvideWork(W),

    /// Result of the work
    DeliverWork(Result<R, WorkError>),
}

#[derive(Debug, Serialize, Deserialize)]
pub struct MessagePacket<W, R> {
    /// The target peer for this message
    /// We could make a more efficient protocol by sending messages directly to the target peer
    /// but given the scale of the project, this is easier
    target_peer_id: Option<Vec<u8>>,

    /// The message
    message: Message<W, R>,
}

#[derive(Debug)]
pub enum Event<W, R> {
    WorkRequested {
        sender: oneshot::Sender<Option<W>>,
    },
    DoWork {
        work: W,
        sender: oneshot::Sender<Result<R, WorkError>>,
    },
    WorkDone {
        result: Result<R, WorkError>,
    },
}

#[derive(Debug, Serialize, Deserialize)]
pub enum WorkError {
    WorkerIsBusy,
    Timeout,
}

// A custom network behaviour that combines Gossipsub and Mdns
#[derive(NetworkBehaviour)]
struct Behaviour {
    mdns: mdns::tokio::Behaviour,
    gossipsub: gossipsub::Behaviour,
}

/// Creates the network components
pub(crate) async fn new<W, R>(
) -> Result<(Client, impl Stream<Item = Event<W, R>>, EventLoop<W, R>), Box<dyn Error>>
where
    W: Serialize + DeserializeOwned,
    R: Serialize + DeserializeOwned,
{
    let mut swarm = libp2p::SwarmBuilder::with_new_identity()
        .with_tokio()
        .with_tcp(
            libp2p::tcp::Config::default(),
            libp2p::noise::Config::new,
            libp2p::yamux::Config::default,
        )?
        .with_quic()
        .with_behaviour(|key| {
            Ok(Behaviour {
                mdns: mdns::tokio::Behaviour::new(
                    mdns::Config::default(),
                    key.public().to_peer_id(),
                )?,
                gossipsub: gossipsub::Behaviour::new(
                    gossipsub::MessageAuthenticity::Signed(key.clone()),
                    gossipsub::ConfigBuilder::default().build()?,
                )?,
            })
        })?
        .with_swarm_config(|cfg| cfg.with_idle_connection_timeout(Duration::from_secs(u64::MAX)))
        .build();

    let default_topic = gossipsub::IdentTopic::new("default");

    // subscribe to the default topic
    swarm.behaviour_mut().gossipsub.subscribe(&default_topic)?;

    // Listen on all interfaces and whatever port the OS assigns
    swarm.listen_on("/ip4/0.0.0.0/udp/0/quic-v1".parse()?)?;
    swarm.listen_on("/ip4/0.0.0.0/tcp/0".parse()?)?;

    let (command_sender, command_receiver) = mpsc::channel(0);
    let (event_sender, event_receiver) = mpsc::channel(0);

    let (publish_sender, publish_receiver) = mpsc::channel(0);

    Ok((
        Client {
            sender: command_sender,
        },
        event_receiver,
        EventLoop {
            swarm,
            command_receiver,
            event_sender,

            publish_sender,
            publish_receiver,

            default_topic,
        },
    ))
}

#[derive(Debug)]
pub enum Command {}

pub struct Client {
    sender: mpsc::Sender<Command>,
}
impl Client {}

pub struct EventLoop<W, R>
where
    W: Serialize + DeserializeOwned,
    R: Serialize + DeserializeOwned,
{
    swarm: Swarm<Behaviour>,
    command_receiver: mpsc::Receiver<Command>,

    /// Sender for events to the outside world
    event_sender: mpsc::Sender<Event<W, R>>,

    publish_sender: mpsc::Sender<MessagePacket<W, R>>,
    publish_receiver: mpsc::Receiver<MessagePacket<W, R>>,

    default_topic: gossipsub::IdentTopic,
}

impl<W, R> EventLoop<W, R>
where
    W: Serialize + DeserializeOwned + Send + 'static,
    R: Serialize + DeserializeOwned + Send + 'static,
{
    pub async fn run(mut self) {
        loop {
            select! {
                _ = tokio::time::sleep(Duration::from_millis(200)) => self.try_request_work().expect("to request work"),
                Some(message) = self.publish_receiver.next() => {self.publish(message).ok();},
                event = self.swarm.select_next_some() => self.handle_behaviour_event(event).await,
                command = self.command_receiver.next() => match command {
                    Some(command) => self.handle_command(command),
                    None => break,
                }
            }
        }
    }

    #[allow(deprecated)] // THandlerErr
    async fn handle_behaviour_event(
        &mut self,
        event: SwarmEvent<BehaviourEvent, THandlerErr<Behaviour>>,
    ) {
        match event {
            SwarmEvent::NewListenAddr { address, .. } => {
                println!("Local node is listening on {}", address);
            }
            SwarmEvent::Behaviour(BehaviourEvent::Mdns(mdns::Event::Discovered(list))) => {
                for (peer_id, _multiaddr) in list {
                    println!("mDNS discovered a new peer: {_multiaddr}");
                    self.swarm
                        .behaviour_mut()
                        .gossipsub
                        .add_explicit_peer(&peer_id);
                }
            }
            SwarmEvent::Behaviour(BehaviourEvent::Mdns(mdns::Event::Expired(list))) => {
                for (peer_id, _multiaddr) in list {
                    println!("mDNS discover peer has expired: {peer_id}");
                    self.swarm
                        .behaviour_mut()
                        .gossipsub
                        .remove_explicit_peer(&peer_id);
                }
            }
            SwarmEvent::Behaviour(BehaviourEvent::Gossipsub(gossipsub::Event::Message {
                propagation_source: _,
                message_id: _,
                message,
            })) => {
                let pkt = serde_json::from_slice::<MessagePacket<W, R>>(message.data.as_slice())
                    .expect("to deserialize message");

                if let Some(target_peer_id) = pkt.target_peer_id {
                    if target_peer_id != self.swarm.local_peer_id().to_bytes() {
                        // not for us
                        return;
                    }
                }

                match pkt.message {
                    Message::RequestWork => {
                        let (sender, receiver) = oneshot::channel();
                        self.event_sender
                            .send(Event::WorkRequested { sender })
                            .await
                            .expect("to send work request event");

                        match receiver.await.expect("to receive work request") {
                            None => {} // no work to do
                            Some(work) => {
                                self.publish(MessagePacket {
                                    target_peer_id: Some(message.source.unwrap().to_bytes()),
                                    message: Message::ProvideWork(work),
                                })
                                .expect("to send work");
                            }
                        }
                    }
                    Message::ProvideWork(work) => {
                        let (sender, receiver) = oneshot::channel();
                        self.event_sender
                            .send(Event::DoWork { work, sender })
                            .await
                            .expect("to send do work event");

                        let mut publish_sender = self.publish_sender.clone();

                        tokio::spawn(async move {
                            let result = receiver.await.expect("to receive work request");
                            //println!("Got work result: {result:?}");

                            let pckt = MessagePacket {
                                target_peer_id: Some(message.source.unwrap().to_bytes()),
                                message: Message::DeliverWork(result),
                            };

                            publish_sender.send(pckt).await.expect("to send work done");
                        });
                    }
                    Message::DeliverWork(result) => {
                        self.event_sender
                            .send(Event::WorkDone { result })
                            .await
                            .expect("to send work done event");
                    }
                }
            }
            _ => {}
        }
    }

    fn handle_command(&mut self, command: Command) {}

    pub fn try_request_work(&mut self) -> Result<(), Box<dyn Error>> {
        match self.publish(MessagePacket {
            target_peer_id: None,
            message: Message::RequestWork,
        }) {
            Ok(_) => {}
            Err(PublishError::InsufficientPeers) => {
                println!("No peers to send request to");
            }
            Err(e) => return Err(Box::new(e)),
        }

        Ok(())
    }

    fn publish(&mut self, message: MessagePacket<W, R>) -> Result<MessageId, PublishError> {
        let message = serde_json::to_vec(&message).expect("to serialize message");
        self.swarm
            .behaviour_mut()
            .gossipsub
            .publish(self.default_topic.clone(), message)
    }
}
