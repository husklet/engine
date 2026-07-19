use hl_engine::{Child, Command, Engine};

fn assert_send<T: Send>() {}

#[test]
fn process_owners_can_move_between_threads() {
    assert_send::<Engine>();
    assert_send::<Command>();
    assert_send::<Child>();
}
