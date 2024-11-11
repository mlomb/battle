use super::swarm::{Event, SwarmNode};

pub struct ConsumerPeer {
    swarm_node: SwarmNode,
}

impl ConsumerPeer {
    pub fn new() -> Self {
        ConsumerPeer {
            swarm_node: SwarmNode::new(),
        }
    }

    pub fn run(mut self) {
        loop {
            match self.swarm_node.next() {
                Event::PeerConnected { peer_id } => {
                    println!("Connected to peer: {:?}", peer_id);
                }
                _ => {}
            }
        }
    }
}
