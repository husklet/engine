`go_cgo_stackgrow.go` and `go_cgo_stackgrow_arm` are exact files from legacy `guests/arm/`.
The committed external-link static AArch64 Go binary remains a fixture; it is never rebuilt by Make.
All C files are exact top-level legacy sources. Non-PIE registrations retain `-static -no-pie`.
