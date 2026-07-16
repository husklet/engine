use crate::Error;
use std::net::Ipv4Addr;

/// Identity shared by guest processes in one virtual network namespace.
#[derive(Clone, Debug, Eq, Hash, PartialEq)]
pub struct Namespace(String);

impl Namespace {
    /// Construct a namespace identity accepted by the engine launch ABI.
    ///
    /// # Errors
    /// Returns an error when the identity is empty, too long, or contains unsupported bytes.
    pub fn new(value: impl Into<String>) -> Result<Self, Error> {
        identity(value.into(), 39, "invalid network namespace identity").map(Self)
    }

    #[must_use]
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

/// Identity of one engine virtual bridge.
#[derive(Clone, Debug, Eq, Hash, PartialEq)]
pub struct Bridge(String);

impl Bridge {
    /// Construct a bridge identity accepted by the engine launch ABI.
    ///
    /// # Errors
    /// Returns an error when the identity is empty, too long, or contains unsupported bytes.
    pub fn new(value: impl Into<String>) -> Result<Self, Error> {
        identity(value.into(), 40, "invalid network bridge identity").map(Self)
    }

    #[must_use]
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

/// One virtual IPv4 interface attached to an engine bridge.
#[derive(Clone, Debug, Eq, Hash, PartialEq)]
pub struct Interface {
    bridge: Bridge,
    address: Ipv4Addr,
    prefix: u8,
}

impl Interface {
    /// Creates an IPv4 interface and its directly connected route.
    ///
    /// # Errors
    /// Returns an error when `prefix` is not a valid IPv4 prefix length.
    pub fn new(bridge: Bridge, address: Ipv4Addr, prefix: u8) -> Result<Self, Error> {
        if prefix > 32 {
            Err(Error::InvalidConfig("IPv4 interface prefix exceeds 32"))
        } else {
            Ok(Self {
                bridge,
                address,
                prefix,
            })
        }
    }

    #[must_use]
    pub fn bridge(&self) -> &Bridge {
        &self.bridge
    }

    #[must_use]
    pub const fn address(&self) -> Ipv4Addr {
        self.address
    }

    #[must_use]
    pub const fn prefix(&self) -> u8 {
        self.prefix
    }
}

/// One host-to-guest TCP/UDP port publication understood by the engine ABI.
///
/// The ABI maps a host address and numeric host port to the virtual network's numeric guest port.
/// Protocol selection remains outside this rule.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct Rule {
    host: u16,
    guest: u16,
    address: Ipv4Addr,
}

impl Rule {
    /// Construct a nonzero `HOST:CONTAINER` publication rule.
    ///
    /// # Errors
    /// Returns an error because port zero is not representable by the current launch ABI.
    pub const fn new(host: u16, guest: u16) -> Result<Self, Error> {
        if host == 0 || guest == 0 {
            Err(Error::InvalidConfig(
                "published host and guest ports must be nonzero",
            ))
        } else {
            Ok(Self {
                host,
                guest,
                address: Ipv4Addr::UNSPECIFIED,
            })
        }
    }

    /// Bind the publication to one exact host IPv4 address.
    #[must_use]
    pub const fn address(mut self, value: Ipv4Addr) -> Self {
        self.address = value;
        self
    }

    #[must_use]
    pub const fn host(self) -> u16 {
        self.host
    }

    #[must_use]
    pub const fn guest(self) -> u16 {
        self.guest
    }

    #[must_use]
    pub const fn host_address(self) -> Ipv4Addr {
        self.address
    }
}

fn identity(value: String, maximum: usize, message: &'static str) -> Result<String, Error> {
    if value.is_empty()
        || value.len() > maximum
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.'))
    {
        Err(Error::InvalidConfig(message))
    } else {
        Ok(value)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn identities_match_the_c_abi_alphabet_and_limits() {
        assert_eq!(
            Namespace::new("net.alpha-1").unwrap().as_str(),
            "net.alpha-1"
        );
        assert_eq!(Bridge::new("_bridge.1").unwrap().as_str(), "_bridge.1");
        assert!(Namespace::new("").is_err());
        assert!(Namespace::new("a".repeat(40)).is_err());
        assert!(Bridge::new("a".repeat(41)).is_err());
        assert!(Bridge::new("bridge/one").is_err());
        assert!(Bridge::new("bridge:one").is_err());
    }

    #[test]
    fn publication_ports_are_nonzero() {
        let rule = Rule::new(8_080, 80).unwrap();
        assert_eq!((rule.host(), rule.guest()), (8_080, 80));
        assert_eq!(rule.host_address(), Ipv4Addr::UNSPECIFIED);
        assert_eq!(
            rule.address(Ipv4Addr::LOCALHOST).host_address(),
            Ipv4Addr::LOCALHOST
        );
        assert!(Rule::new(0, 80).is_err());
        assert!(Rule::new(8_080, 0).is_err());
    }
}
