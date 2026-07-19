use super::{
    Arc, AtomicBool, BTreeMap, Channel, Duration, Frame, Instant, MessageType, Mutex, Ordering,
    ProviderDispatcher, TransportError,
};
pub(crate) struct ServiceServer {
    channel: Arc<Channel>,
    dispatcher: Arc<ProviderDispatcher>,
    active: Arc<Mutex<BTreeMap<u64, Arc<AtomicBool>>>>,
    maximum_active: u32,
    request_timeout: Duration,
}

impl ServiceServer {
    pub(crate) fn new(
        channel: Arc<Channel>,
        dispatcher: Arc<ProviderDispatcher>,
        maximum_active: u32,
        request_timeout: Duration,
    ) -> Self {
        Self {
            channel,
            dispatcher,
            active: Arc::new(Mutex::new(BTreeMap::new())),
            maximum_active,
            request_timeout,
        }
    }

    pub(crate) fn run(&self, deadline: Instant) -> Result<(), TransportError> {
        loop {
            let frame = match self.channel.receive(deadline) {
                Ok(frame) => frame,
                Err(TransportError::PeerClosed) => {
                    self.cancel_all();
                    self.dispatcher.close_all();
                    return Ok(());
                }
                Err(error) => return Err(error),
            };
            match frame.kind {
                MessageType::Request | MessageType::Subscribe => self.start(frame)?,
                MessageType::Cancel | MessageType::Unsubscribe => self.cancel(frame.request_id)?,
                MessageType::Close => {
                    self.cancel_all();
                    self.dispatcher.close_all();
                    return Ok(());
                }
                _ => return Err(TransportError::Malformed),
            }
        }
    }

    fn start(&self, frame: Frame) -> Result<(), TransportError> {
        if frame.request_id == 0 {
            return Err(TransportError::Malformed);
        }
        let reply_kind = if frame.kind == MessageType::Subscribe {
            MessageType::ReadinessEvent
        } else {
            MessageType::Reply
        };
        let cancelled = Arc::new(AtomicBool::new(false));
        {
            let mut active = self.active.lock().map_err(|_| TransportError::Io)?;
            if active.len() >= self.maximum_active as usize {
                return Err(TransportError::Quota);
            }
            if active.insert(frame.request_id, cancelled.clone()).is_some() {
                return Err(TransportError::Malformed);
            }
        }
        let channel = self.channel.clone();
        let dispatcher = self.dispatcher.clone();
        let active = self.active.clone();
        let timeout = self.request_timeout;
        std::thread::spawn(move || {
            let deadline = Instant::now() + timeout;
            let reply = dispatcher.dispatch(&frame.payload, deadline);
            let was_cancelled = cancelled.load(Ordering::Acquire);
            if let Ok(mut values) = active.lock() {
                values.remove(&frame.request_id);
            }
            if !was_cancelled {
                if let Ok(payload) = reply {
                    let _ = channel.send(
                        &Frame {
                            kind: reply_kind,
                            request_id: frame.request_id,
                            features: 0,
                            payload,
                        },
                        deadline,
                    );
                }
            }
        });
        Ok(())
    }

    fn cancel(&self, request_id: u64) -> Result<(), TransportError> {
        if request_id == 0 {
            return Err(TransportError::Malformed);
        }
        if let Some(cancelled) = self
            .active
            .lock()
            .map_err(|_| TransportError::Io)?
            .get(&request_id)
        {
            cancelled.store(true, Ordering::Release);
        }
        Ok(())
    }

    fn cancel_all(&self) {
        if let Ok(active) = self.active.lock() {
            for cancelled in active.values() {
                cancelled.store(true, Ordering::Release);
            }
        }
    }
}
