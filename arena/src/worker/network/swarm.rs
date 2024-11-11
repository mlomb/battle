use super::message::MessageRequest;
use super::message::MessageResponse;
use futures::channel::mpsc;
use futures::channel::oneshot;
use futures::prelude::*;
use futures::StreamExt;
use libp2p::gossipsub::{MessageId, PublishError};
use libp2p::request_response;
#[allow(deprecated)]
use libp2p::swarm::{SwarmEvent, THandlerErr};
use libp2p::Multiaddr;
use libp2p::PeerId;
use libp2p::StreamProtocol;
use libp2p::Swarm;
use libp2p::{gossipsub, mdns, swarm::NetworkBehaviour};
use serde::de::DeserializeOwned;
use serde::Deserialize;
use serde::Serialize;
use std::error::Error;
use std::time::Duration;
use tokio::runtime::Runtime;
use tokio::select;

#[derive(Debug)]
pub enum Event {
    /// The local node is now listening on the given multiaddr.
    ListeningOn { address: Multiaddr },
    /// A connection to a peer has been established.
    PeerConnected { peer_id: PeerId },
    /// A message request has been received from a peer.
    MessageRequestReceived {
        peer_id: PeerId,
        message: MessageRequest,
        sender: oneshot::Sender<MessageResponse>,
    },
    /// A message response has been received from a peer.
    MessageResponseReceived {
        peer_id: PeerId,
        message: MessageResponse,
    },
}

#[derive(Debug)]
pub enum Command {
    SendRequest {
        peer_id: PeerId,
        request: MessageRequest,
    },
}

// A custom network behaviour that combines mDNS with RequestResponse
#[derive(NetworkBehaviour)]
struct Behaviour {
    mdns: mdns::tokio::Behaviour,
    request_response: request_response::cbor::Behaviour<MessageRequest, MessageResponse>,
}

/// A node in a libp2p Swarm. It allows sending and receiving network messages.
pub struct SwarmNode {
    /// The Tokio runtime in which the Swarm loop is running
    #[allow(dead_code)]
    runtime: Runtime,

    /// Sender for commands to the Swarm loop
    command_sender: mpsc::Sender<Command>,

    /// Receiver for events from the Swarm loop
    event_receiver: std::sync::mpsc::Receiver<Event>,
}

impl SwarmNode {
    /// Spins up a new node in the network.
    /// Initializes a Tokio runtime with the event loop in it.
    pub fn new() -> SwarmNode {
        let runtime = Runtime::new().unwrap();
        let (command_sender, command_receiver) = mpsc::channel(0);
        let (event_sender, event_receiver) = std::sync::mpsc::channel();

        runtime.spawn(async move {
            let mut swarm = libp2p::SwarmBuilder::with_new_identity()
                .with_tokio()
                .with_tcp(
                    libp2p::tcp::Config::default(),
                    libp2p::noise::Config::new,
                    libp2p::yamux::Config::default,
                )
                .unwrap()
                .with_quic()
                .with_behaviour(|key| {
                    Ok(Behaviour {
                        mdns: mdns::tokio::Behaviour::new(
                            mdns::Config::default(),
                            key.public().to_peer_id(),
                        )?,
                        request_response: request_response::cbor::Behaviour::new(
                            [(
                                StreamProtocol::new("/mlomb/bot-tools/arena/1"),
                                request_response::ProtocolSupport::Full,
                            )],
                            request_response::Config::default(),
                        ),
                    })
                })
                .unwrap()
                .with_swarm_config(|cfg| {
                    cfg.with_idle_connection_timeout(Duration::from_secs(u64::MAX))
                })
                .build();

            // Listen on all interfaces and whatever port the OS assigns
            swarm
                .listen_on("/ip4/0.0.0.0/tcp/0".parse().unwrap())
                .unwrap();

            let network_loop = EventLoop {
                swarm,
                command_receiver,
                event_sender,
            };
            network_loop.run().await;
        });

        SwarmNode {
            runtime,
            command_sender,
            event_receiver,
        }
    }

    pub fn next(&mut self) -> Event {
        self.event_receiver.recv().unwrap()
    }

    pub fn send(&self, peer_id: PeerId, request: MessageRequest) {
        self.command_sender
            .clone()
            .try_send(Command::SendRequest { peer_id, request })
            .unwrap();
    }
}

pub struct EventLoop {
    /// The libp2p Swarm
    swarm: Swarm<Behaviour>,

    /// Receiver for commands from the outside world
    command_receiver: mpsc::Receiver<Command>,

    /// Sender for events to the outside world
    event_sender: std::sync::mpsc::Sender<Event>,
}

impl EventLoop {
    pub async fn run(mut self) {
        loop {
            select! {
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
                self.event_sender
                    .send(Event::ListeningOn { address })
                    .unwrap();
            }
            SwarmEvent::Behaviour(BehaviourEvent::Mdns(mdns::Event::Discovered(list))) => {
                for (peer_id, _multiaddr) in list {
                    self.swarm.dial(peer_id).ok();
                }
            }
            SwarmEvent::ConnectionEstablished { peer_id, .. } => {
                self.event_sender
                    .send(Event::PeerConnected { peer_id })
                    .unwrap();
            }
            SwarmEvent::Behaviour(BehaviourEvent::RequestResponse(rr)) => match rr {
                request_response::Event::Message { peer, message } => match message {
                    request_response::Message::Request {
                        request_id,
                        request,
                        channel,
                    } => {
                        let (sender, receiver) = oneshot::channel();
                        self.event_sender
                            .send(Event::MessageRequestReceived {
                                peer_id: peer,
                                message: request,
                                sender,
                            })
                            .unwrap();
                        let response = receiver.await.unwrap();
                        self.swarm
                            .behaviour_mut()
                            .request_response
                            .send_response(channel, response)
                            .unwrap();
                    }
                    request_response::Message::Response {
                        request_id,
                        response,
                    } => {
                        self.event_sender
                            .send(Event::MessageResponseReceived {
                                peer_id: peer,
                                message: response,
                            })
                            .unwrap();
                    }
                },
                request_response::Event::OutboundFailure {
                    peer,
                    request_id,
                    error,
                } => {
                    println!("Outbound failure: {:?}", error);
                }
                request_response::Event::InboundFailure {
                    peer,
                    request_id,
                    error,
                } => {
                    println!("Inbound failure: {:?}", error);
                }
                request_response::Event::ResponseSent { .. } => {}
            },
            a => {
                println!("Unhandled event: {:?}", a);
            }
        }
    }

    fn handle_command(&mut self, command: Command) {
        match command {
            Command::SendRequest { peer_id, request } => {
                self.swarm
                    .behaviour_mut()
                    .request_response
                    .send_request(&peer_id, request);
            }
        }
    }
}
