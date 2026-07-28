//! Linux namespace isolation selection.

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct NamespaceSpec {
    pub mount: IsolationMode,
    pub pid: IsolationMode,
    pub uts: IsolationMode,
    pub ipc: IsolationMode,
    pub network: IsolationMode,
    pub user: IsolationMode,
    pub cgroup: IsolationMode,
}
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub enum IsolationMode {
    #[default]
    Private,
    Host,
    Shared(NamespaceHandle),
}
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct NamespaceHandle(pub u64);
