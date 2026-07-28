use std::collections::BTreeSet;

#[derive(Clone, Debug, Default)]
pub(super) struct Participant {
    pub(super) host_pid: u64,
    pub(super) last_op: &'static str,
    pub(super) open_groups: BTreeSet<String>,
    pub(super) committed_groups: BTreeSet<String>,
    pub(super) aborted_groups: BTreeSet<String>,
}

#[derive(Clone, Debug, Default)]
pub(crate) struct Progress {
    pub(crate) participants: usize,
    pub(crate) incomplete: Vec<String>,
    pub(crate) aborted: Vec<String>,
    pub(crate) committed: Vec<String>,
    pub(crate) detail: Vec<String>,
}

impl Progress {
    pub(crate) fn any_image_started(&self) -> bool {
        !self.incomplete.is_empty() || !self.aborted.is_empty() || !self.committed.is_empty()
    }
}

impl std::fmt::Display for Progress {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            formatter,
            "{} engine process(es) attached",
            self.participants
        )?;
        for (label, names) in [
            ("committed", &self.committed),
            ("still open", &self.incomplete),
            ("aborted", &self.aborted),
        ] {
            if !names.is_empty() {
                write!(formatter, "; {label} {}", names.join(", "))?;
            }
        }
        if !self.detail.is_empty() {
            write!(formatter, "; {}", self.detail.join(", "))?;
        }
        Ok(())
    }
}
