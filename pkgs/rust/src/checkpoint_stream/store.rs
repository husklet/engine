use std::{collections::BTreeMap, sync::Mutex};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct StoreError {
    pub message: String,
}

impl StoreError {
    pub fn new(message: impl Into<String>) -> Self {
        Self {
            message: message.into(),
        }
    }
}

impl std::fmt::Display for StoreError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(&self.message)
    }
}

impl std::error::Error for StoreError {}

pub trait CheckpointStore: Send + Sync {
    /// Stores one complete object.
    ///
    /// # Errors
    /// Any error fails the capture.
    fn put(&self, name: &str, data: &[u8]) -> Result<(), StoreError>;

    /// Reads one object.
    ///
    /// # Errors
    /// Returns an error when the object cannot be read.
    fn get(&self, name: &str) -> Result<Vec<u8>, StoreError>;

    /// Lists every object name.
    ///
    /// # Errors
    /// Returns an error when the store cannot be enumerated.
    fn list(&self) -> Result<Vec<String>, StoreError>;

    /// Publishes the final manifest.
    ///
    /// # Errors
    /// Any error fails the capture.
    fn commit(&self, manifest: &[u8]) -> Result<(), StoreError> {
        self.put("MANIFEST", manifest)
    }
}

#[derive(Debug, Default)]
pub struct MemoryStore {
    objects: Mutex<BTreeMap<String, Vec<u8>>>,
}

impl MemoryStore {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    #[must_use]
    /// # Panics
    /// Panics when the store lock is poisoned.
    pub fn objects(&self) -> BTreeMap<String, Vec<u8>> {
        self.objects.lock().expect("memory store lock").clone()
    }

    #[must_use]
    /// # Panics
    /// Panics when the store lock is poisoned.
    pub fn committed(&self) -> bool {
        self.objects
            .lock()
            .expect("memory store lock")
            .contains_key("MANIFEST")
    }

    #[must_use]
    /// # Panics
    /// Panics when the store lock is poisoned.
    pub fn bytes(&self) -> usize {
        self.objects
            .lock()
            .expect("memory store lock")
            .values()
            .map(Vec::len)
            .sum()
    }
}

impl CheckpointStore for MemoryStore {
    fn put(&self, name: &str, data: &[u8]) -> Result<(), StoreError> {
        self.objects
            .lock()
            .map_err(|_| StoreError::new("memory store lock is poisoned"))?
            .insert(name.to_owned(), data.to_vec());
        Ok(())
    }

    fn get(&self, name: &str) -> Result<Vec<u8>, StoreError> {
        self.objects
            .lock()
            .map_err(|_| StoreError::new("memory store lock is poisoned"))?
            .get(name)
            .cloned()
            .ok_or_else(|| StoreError::new(format!("no such object: {name}")))
    }

    fn list(&self) -> Result<Vec<String>, StoreError> {
        Ok(self
            .objects
            .lock()
            .map_err(|_| StoreError::new("memory store lock is poisoned"))?
            .keys()
            .cloned()
            .collect())
    }
}
