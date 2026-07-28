use std::collections::BTreeMap;

const BASIS: u64 = 14_695_981_039_346_656_037;
const PRIME: u64 = 1_099_511_628_211;

pub(super) struct CheckpointDigest;

impl CheckpointDigest {
    fn extend(mut hash: u64, data: &[u8]) -> u64 {
        for byte in data {
            hash ^= u64::from(*byte);
            hash = hash.wrapping_mul(PRIME);
        }
        hash
    }

    pub(super) fn object(name: &str, data: &[u8]) -> u64 {
        let mut hash = Self::extend(BASIS, name.as_bytes());
        hash = Self::extend(hash, &[0]);
        hash = Self::extend(hash, &(data.len() as u64).to_ne_bytes());
        Self::extend(hash, data)
    }

    pub(super) fn image(objects: &BTreeMap<String, (u64, u64)>) -> (u64, u64, u64) {
        let mut hash = BASIS;
        let mut bytes = 0_u64;
        for (name, (object, size)) in objects {
            hash = Self::extend(hash, name.as_bytes());
            hash = Self::extend(hash, &[0]);
            hash = Self::extend(hash, &object.to_ne_bytes());
            bytes += *size;
        }
        (hash, objects.len() as u64, bytes)
    }

    pub(super) fn includes(name: &str) -> bool {
        name != "MANIFEST" && name != "RECOVERY.jsonl" && !name.starts_with(".RECOVERY.jsonl.tmp.")
    }
}
