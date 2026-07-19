use super::SpecError;

#[derive(Debug)]
pub enum SpawnError {
    Spec(SpecError),
    Engine(crate::Error),
}

impl std::fmt::Display for SpawnError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Spec(error) => error.fmt(formatter),
            Self::Engine(error) => error.fmt(formatter),
        }
    }
}

impl std::error::Error for SpawnError {}
