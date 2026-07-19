use crate::network;

use super::NetworkMode;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct NetworkSpec {
    pub mode: NetworkMode,
    pub namespace: Option<network::Namespace>,
    pub interfaces: Vec<network::Interface>,
    pub port_forwards: Vec<network::Rule>,
    pub external_listeners: bool,
}

impl Default for NetworkSpec {
    fn default() -> Self {
        Self {
            mode: NetworkMode::Host,
            namespace: None,
            interfaces: Vec::new(),
            port_forwards: Vec::new(),
            external_listeners: false,
        }
    }
}
