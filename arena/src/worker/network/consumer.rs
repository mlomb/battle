use super::swarm::{Event, SwarmNode};
use crate::worker::network::message::{MessageRequest, MessageResponse};
use libp2p::PeerId;
use std::collections::{hash_map::Entry, HashMap};

pub struct ConsumerPeer {
    swarm_node: SwarmNode,

    peers: HashMap<PeerId, Peer>,
}

impl ConsumerPeer {
    pub fn new() -> Self {
        ConsumerPeer {
            swarm_node: SwarmNode::new(),
            peers: HashMap::new(),
        }
    }

    pub fn run(mut self) {
        loop {
            match self.swarm_node.next() {
                Event::ListeningOn { address: multiaddr } => {
                    println!("Listening on: {:?}", multiaddr);
                }
                Event::PeerConnected { peer_id } => match self.peers.entry(peer_id) {
                    Entry::Occupied(_) => {}
                    Entry::Vacant(v) => {
                        println!("Connected to peer: {:?}. Requesting Env...", peer_id);

                        // add to the list of known peers
                        v.insert(Peer { peer_id });

                        // request the env from the peer (if it has one)
                        self.swarm_node.send(peer_id, MessageRequest::ProvideEnv);
                    }
                },
                Event::MessageRequestReceived {
                    peer_id: _,
                    message,
                    sender,
                } => match message {
                    MessageRequest::ProvideEnv => {
                        // this peer is a consumer, so it has no Env
                        sender.send(MessageResponse::EnvNotProvided).ok();
                    }
                },
                Event::MessageResponseReceived { peer_id, message } => match message {
                    MessageResponse::EnvProvided { env } => {
                        println!("{:?}", env);
                        println!("Received Env from peer: {:?}", peer_id);
                    }
                    MessageResponse::EnvNotProvided => {
                        println!("Peer {:?} is a consumer", peer_id);
                    }
                    MessageResponse::Dummy => {}
                },
                _ => {}
            }
        }
    }
}

struct Peer {
    peer_id: PeerId,
}
