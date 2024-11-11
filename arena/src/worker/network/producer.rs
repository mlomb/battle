use std::path::PathBuf;

use serde::Serialize;

use crate::{env::Env, referee::Referee, worker::network::message::MessageResponse};

use super::{
    message::MessageRequest,
    swarm::{Event, SwarmNode},
};

pub struct ProducerPeer {
    swarm_node: SwarmNode,
}

impl ProducerPeer {
    pub fn new() -> Self {
        ProducerPeer {
            swarm_node: SwarmNode::new(),
        }
    }

    pub fn run(mut self) {
        loop {
            match self.swarm_node.next() {
                Event::PeerConnected { peer_id } => {
                    println!("Connected to peer: {:?}", peer_id);
                }
                Event::MessageRequestReceived {
                    peer_id: _,
                    message,
                    sender,
                } => match message {
                    MessageRequest::ProvideEnv => {
                        println!("Received request for Env");
                        let env = Env::from_file(&PathBuf::from("./env.yml")).unwrap();

                        // https://github.com/libp2p/rust-libp2p/issues/5383
                        // the env is a producer, so it has an Env
                        sender.send(MessageResponse::EnvProvided { env }).unwrap();

                        println!("Env sent");
                    }
                },
                _ => {}
            }
        }
    }
}
