# Isolation compatibility suite

This suite transfers the seven legacy `ext_iso` guests byte-for-byte and records every bare-guest registration from `ext/isolation.rs`. Configuration variants run through ABI4 launch fields. Normalized outputs are checked exactly and across both production ISAs; image-rootfs shell registrations remain explicit external-service cases in `images.tsv`.
