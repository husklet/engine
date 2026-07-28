use super::{CheckpointDigest, CheckpointStore, MemoryStore};
use std::collections::BTreeMap;

#[test]
fn the_image_digest_is_order_independent_in_its_input() {
    let mut forward = BTreeMap::new();
    forward.insert("a".to_owned(), (CheckpointDigest::object("a", b"one"), 3));
    forward.insert("b".to_owned(), (CheckpointDigest::object("b", b"two"), 3));
    let mut backward = BTreeMap::new();
    backward.insert("b".to_owned(), (CheckpointDigest::object("b", b"two"), 3));
    backward.insert("a".to_owned(), (CheckpointDigest::object("a", b"one"), 3));
    assert_eq!(
        CheckpointDigest::image(&forward),
        CheckpointDigest::image(&backward)
    );
    assert_eq!(CheckpointDigest::image(&forward).1, 2);
    assert_eq!(CheckpointDigest::image(&forward).2, 6);
}

#[test]
fn a_memory_store_round_trips_objects() {
    let store = MemoryStore::new();
    store.put("proc.1/pages", b"payload").expect("put");
    assert_eq!(store.get("proc.1/pages").expect("get"), b"payload");
    assert_eq!(store.list().expect("list"), vec!["proc.1/pages".to_owned()]);
    assert!(!store.committed());
    store.commit(b"manifest").expect("commit");
    assert!(store.committed());
}
