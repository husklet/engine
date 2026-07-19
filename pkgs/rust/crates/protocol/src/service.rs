mod codec;
mod input;
mod model;
mod namespace;

pub use codec::{decode_reply, decode_request, encode_reply, encode_request};
pub use model::{Reply, Request, SeekWhence, ServiceFailure, ServiceStat};
pub use namespace::{
    decode_namespace_install, encode_namespace_install, ProjectionKind, ServiceProjection,
};

#[cfg(test)]
mod tests {
    use super::{
        codec::WRITE, decode_namespace_install, decode_reply, decode_request,
        encode_namespace_install, encode_reply, encode_request, input::protocol, ProjectionKind,
        Reply, Request, ServiceFailure, ServiceProjection,
    };
    use hl_engine_api::extension::ServiceId;
    use hl_engine_provider::{LinuxError, Readiness, ReadyState};

    #[test]
    fn frozen_codec_round_trips_requests_replies_and_errno() {
        let request = Request::Write {
            handle: 7,
            offset: 11,
            bytes: b"owned".to_vec(),
        };
        let encoded = encode_request(&request, 16).unwrap();
        assert_eq!(
            encoded,
            [
                vec![WRITE],
                7_u64.to_le_bytes().to_vec(),
                11_u64.to_le_bytes().to_vec(),
                5_u32.to_le_bytes().to_vec(),
                b"owned".to_vec(),
            ]
            .concat()
        );
        assert_eq!(decode_request(&encoded, 16).unwrap(), request);

        let ready = Reply::Ready(Readiness {
            states: [ReadyState::Readable, ReadyState::Hangup]
                .into_iter()
                .collect(),
        });
        let encoded = encode_reply(&Ok(ready.clone()), 16).unwrap();
        assert_eq!(decode_reply(&encoded, 16).unwrap(), ready);

        let failure = ServiceFailure::Linux(LinuxError {
            errno: 19,
            context: "provider unavailable".into(),
        });
        let encoded = encode_reply(&Err(failure.clone()), 64).unwrap();
        assert_eq!(decode_reply(&encoded, 64).unwrap_err(), failure);
    }

    #[test]
    fn codecs_reject_trailing_invalid_and_oversized_payloads() {
        let mut request = encode_request(&Request::Close { handle: 1 }, 8).unwrap();
        request.push(0);
        assert_eq!(decode_request(&request, 8).unwrap_err(), protocol());
        assert_eq!(decode_request(&[WRITE], 8).unwrap_err(), protocol());
        assert!(matches!(
            encode_request(
                &Request::Write {
                    handle: 1,
                    offset: 0,
                    bytes: vec![0; 9],
                },
                8,
            ),
            Err(ServiceFailure::Linux(LinuxError { errno: 22, .. }))
        ));
    }

    #[test]
    fn namespace_codec_enforces_transaction_bounds_and_normalization() {
        let entries = vec![ServiceProjection {
            path: "/run/provider".into(),
            service: ServiceId(9),
            mode: 0o660,
            uid: 10,
            gid: 20,
            kind: ProjectionKind::Service,
        }];
        let wire = encode_namespace_install(&entries, 4, 128).unwrap();
        assert_eq!(decode_namespace_install(&wire, 4, 128).unwrap(), entries);

        let devices = vec![ServiceProjection {
            path: "/dev/provider".into(),
            service: ServiceId(10),
            mode: 0o660,
            uid: 11,
            gid: 12,
            kind: ProjectionKind::CharacterDevice {
                major: 226,
                minor: 128,
            },
        }];
        let wire = encode_namespace_install(&devices, 4, 128).unwrap();
        assert_eq!(decode_namespace_install(&wire, 4, 128).unwrap(), devices);

        let conflicts = vec![
            ServiceProjection {
                path: "/run/provider".into(),
                service: ServiceId(1),
                mode: 0o600,
                uid: 0,
                gid: 0,
                kind: ProjectionKind::Service,
            },
            ServiceProjection {
                path: "/run/provider/child".into(),
                service: ServiceId(2),
                mode: 0o600,
                uid: 0,
                gid: 0,
                kind: ProjectionKind::Service,
            },
        ];
        assert!(matches!(
            encode_namespace_install(&conflicts, 4, 128),
            Err(ServiceFailure::Linux(LinuxError { errno: 20, .. }))
        ));

        let mut trailing = encode_namespace_install(&entries, 4, 128).unwrap();
        trailing.push(0);
        assert_eq!(
            decode_namespace_install(&trailing, 4, 128).unwrap_err(),
            protocol()
        );
    }
}
