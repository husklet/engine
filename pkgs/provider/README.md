# HL Engine Provider

`hl-engine-provider` defines live ports implemented by extension providers and
consumed by engine backends. It depends on the declarative `hl-engine-api`
contracts and owns no engine backend, native asset, or product-specific policy.

The crate is organized around the live provider ports: preparation and manifests,
namespace installation, open handles, memory resources, events/lifecycle, and
launch authority with typed failures. The crate root reexports those contracts so
existing type paths and identities remain stable.
