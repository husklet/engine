use std::collections::{BTreeMap, HashMap, HashSet};

use super::Participant;

#[derive(Debug)]
pub(super) struct Object {
    pub(super) name: String,
    pub(super) bytes: Vec<u8>,
}

#[derive(Default)]
pub(super) struct State {
    pub(super) participants: BTreeMap<u64, Participant>,
    pub(super) open: HashMap<(u64, u64), Object>,
    pub(super) staged: HashMap<String, Vec<Object>>,
    pub(super) committed_groups: HashSet<String>,
    pub(super) claims: HashSet<String>,
    pub(super) digest: BTreeMap<String, (u64, u64)>,
    pub(super) failure: Option<String>,
}
