{
  description = "hl-engine: Linux execution engine and language bindings";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { self, nixpkgs }:
    let
      lib = nixpkgs.lib;
      version = "0.1.25";

      systems = [ "aarch64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = function:
        lib.genAttrs systems (system: function (import nixpkgs {
          inherit system;
          config.allowUnsupportedSystem = true;
        }));

      # =====================================================================
      # The project has three independent axes (DOCS.md section 1): guest OS
      # (Linux), guest ISA, and host platform. Modelling the last two as DATA
      # keeps the flake from growing a new `system == "..."` branch every time
      # one of them gains a member -- which is how it previously ended up with
      # the whole compiler/loader environment written out once per host.
      # =====================================================================

      # Guest ISAs the engine translates. Adding one is a single entry here:
      # every *_LINUX_CC / *_STATIC_CC / *_DYNAMIC_* variable below is derived.
      guestISAs = [
        { isa = "aarch64"; crossAttr = "aarch64-multiplatform"; loader = "ld-linux-aarch64.so.1"; }
        { isa = "x86_64"; crossAttr = "gnu64"; loader = "ld-linux-x86-64.so.2"; }
      ];

      # Host backends implementing the hl_host_services contract, one per
      # src/host/<name>/. `supported` gates whether outputs are produced at all:
      # src/host/windows/ is currently a reserved boundary (README only, no code
      # and no Makefile/CMake wiring), so a windows host evaluates but builds
      # nothing. When that backend lands, flip the flag.
      hostBackends = {
        linux = { supported = true; };
        macos = { supported = true; };
        windows = { supported = false; };
      };

      # Everything host- and guest-dependent, derived once from `pkgs`.
      toolchainFor = pkgs:
        let
          host = pkgs.stdenv.hostPlatform;
          hostCpu = host.parsed.cpu.name; # "aarch64" | "x86_64"
          backendName = if host.isDarwin then "macos" else if host.isLinux then "linux" else "windows";
          nativeCC = "${pkgs.stdenv.cc}/bin/cc";

          # A guest ISA is built natively only when the host runs Linux on that
          # same ISA -- otherwise it needs a cross toolchain. This is the whole
          # host/ISA relationship, and replaces the old `isLinuxHost` flag that
          # conflated "host is Linux" with "host is aarch64-Linux" (and so gave
          # x86_64-linux the wrong, fully-cross treatment).
          isNative = g: host.isLinux && hostCpu == g.isa;
          pkgsFor = g: if isNative g then pkgs else pkgs.pkgsCross.${g.crossAttr};
          ccFor = g:
            let p = pkgsFor g;
            in if isNative g then nativeCC else "${p.stdenv.cc}/bin/${p.stdenv.cc.targetPrefix}cc";
          # Guest binaries link a STATIC sqlite and glibc for their own ISA; the
          # -I/-L pair is what the compat-core dbserver/sqlite workloads and the
          # combined-bench sqlite phase need.
          #
          # This deliberately uses the STOCK pkgsStatic.sqlite even though that is
          # a musl stdenv and therefore implies a musl cross toolchain per guest
          # ISA. Two alternatives were tried and are worse:
          #   * moving the -I/-L pair to a separate perf-only shell -- wrong,
          #     because the aarch64 dbserver/sqlite workloads are compat-core
          #     cases that CI builds, not perf targets;
          #   * overriding the ordinary glibc sqlite to build a static archive --
          #     leaves the binary cache, forcing a from-source rebuild of the
          #     whole chain including tcl, which does not cross-compile for
          #     x86_64 (implicit strlen declaration is an error on modern gcc).
          # The stock path is cached. A long musl toolchain build usually means
          # the nix/CI cache key changed (it hashes flake.nix), not a new
          # dependency.
          staticCCFor = g:
            let p = pkgsFor g;
            in "${ccFor g} -I${lib.getDev p.pkgsStatic.sqlite}/include"
               + " -L${lib.getLib p.pkgsStatic.sqlite}/lib -L${p.glibc.static}/lib";
          # The compiler *packages* (not paths) that belong on PATH in a shell.
          ccPkgFor = g: if isNative g then pkgs.gcc else (pkgsFor g).stdenv.cc;

          upper = g: lib.toUpper g.isa; # aarch64 -> AARCH64, x86_64 -> X86_64
        in
        rec {
          inherit host hostCpu nativeCC isNative pkgsFor;
          backend = hostBackends.${backendName};
          # Can this host build guest fixtures at all?
          canBuildGuests = backend.supported;
          # Hosts that can execute a built engine, and therefore get the Rust
          # package. A predicate, not an allowlist of system strings.
          canRunGuests = backend.supported && hostCpu == "aarch64";

          crossCompilers = map ccPkgFor guestISAs;

          # Exactly the variable names the Makefile and cmake/toolchains/* read.
          # Note the two distinct prefixes that already exist in the tree:
          # <ISA>_LINUX_* for compilers, <ISA>_DYNAMIC_* for loader/libc.
          env = lib.foldl'
            (acc: g: acc // {
              "${upper g}_LINUX_CC" = ccFor g;
              "${upper g}_LINUX_STATIC_CC" = staticCCFor g;
              "${upper g}_DYNAMIC_LOADER" = "${(pkgsFor g).glibc}/lib/${g.loader}";
              "${upper g}_DYNAMIC_LIBC" = "${(pkgsFor g).glibc}/lib/libc.so.6";
            })
            { CC = nativeCC; NATIVE_CC = nativeCC; }
            guestISAs;

        };

      # Source for the C build. `cleanSource` drops VCS noise; the build trees
      # are named so they cannot be confused with the Makefile's `build/`.
      engineSrc = lib.cleanSource self;

      # A CMake-configured derivation.
      #
      # cmakeBuildType MUST be "None", not "": CMakeLists.txt specifies the exact
      # flag sets and CMake must not inject an -O level or -DNDEBUG of its own,
      # but nixpkgs' cmake hook expands ${cmakeBuildType:-Release} in bash, so an
      # EMPTY string silently becomes Release (-O3 -DNDEBUG). "None" is the CMake
      # build type that genuinely adds nothing.
      cmakeDrv = pkgs: args: pkgs.stdenv.mkDerivation ({
        src = engineSrc;
        strictDeps = true;
        cmakeBuildType = "None";
        nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ]
          ++ (args.extraNativeBuildInputs or [ ]);
      } // (removeAttrs args [ "extraNativeBuildInputs" ]));
    in
    {
      packages = forAllSystems (pkgs:
        let tc = toolchainFor pkgs; in
        lib.optionalAttrs tc.canBuildGuests {
          # `nix build` -- nix drives CMake, CMake owns the build graph. This
          # replaced a make-driven derivation; CI gates on checks.package, so
          # what CI validates is now the CMake build.
          #
          # No toolchain file is passed: inside a nix build the stdenv compiler
          # IS the intended host compiler. cmake/toolchains/* exist for genuine
          # cross builds and the remote-macOS lane, not for native ones.
          default = cmakeDrv pkgs {
            pname = "hl-engine";
            inherit version;
            # Libraries, the runner, the per-guest-ISA production engines and
            # the embedded activation archive -- the artifacts an embedder uses.
            ninjaFlags = [ "hl-engine" "hl-translator" "hl-linux-abi" "hl-host-fake" ]
              ++ lib.optionals pkgs.stdenv.hostPlatform.isLinux [
                "hl-host-linux"
                "hl-engine-runner"
                "hl-engine-activation"
                "hl-engine-linux-aarch64"
                "hl-engine-linux-x86_64"
              ];
            doCheck = false; # covered by checks.unit, which is not sandbox-hostile
            # Every mac executable is codesigned with packaging/macos/jit.entitlements at link time,
            # because the JIT's RX alias is non-MAP_JIT executable memory and a SIP-on host SIGKILLs an
            # unentitled process that runs it. nixpkgs' fixupPhase strips binaries AFTER that signature
            # exists, which invalidates it -- nix then re-signs ad-hoc, silently WITHOUT entitlements, so
            # `codesign -d --entitlements -` on the installed runner came back empty and the binary would
            # have died at runtime with no diagnostic. Keep the signed binary intact on darwin.
            dontStrip = pkgs.stdenv.hostPlatform.isDarwin;
            meta = {
              description = "Userspace execution engine for Linux programs";
              homepage = "https://github.com/husklet/engine";
              license = lib.licenses.mit;
              platforms = systems;
              mainProgram = "hl-engine-runner";
            };
          };
        } // lib.optionalAttrs tc.canRunGuests {
          rust = pkgs.rustPlatform.buildRustPackage {
            pname = "hl-engine";
            inherit version;
            # The publishable `hl-engine` crate lives at pkgs/rust; its former
            # api/provider/protocol/runtime member crates are now internal
            # modules, so the whole pkgs/rust tree is the sandbox.
            src = lib.cleanSourceWith {
              src = ./pkgs/rust;
              filter = path: type:
                let base = baseNameOf (toString path);
                in !(type == "directory" && base == "target");
            };
            cargoLock.lockFile = ./pkgs/rust/Cargo.lock;
            nativeBuildInputs = [ pkgs.pkg-config ];
            # Native-linking test binaries are skipped: they link the frozen
            # libhl-engine.a activation archive, whose refresh is a separately
            # gated frozen-asset publication (pkgs/rust/assets/PROVENANCE.md),
            # not part of the structural build this check guards.
            doCheck = false;
          };
        });

      # `nix flake check` is the verification entry point. These are
      # DERIVATIONS on purpose: a nix cc-wrapper needs NIX_CFLAGS_COMPILE /
      # NIX_CC / the hardening variables, which only a real stdenv provides.
      # The same work expressed as a `nix run` script fails to compile (it
      # surfaced as an implicit-declaration error for realpath), so anything
      # that invokes a compiler belongs here or in the devShell -- never in a
      # writeShellApplication.
      checks = forAllSystems (pkgs:
        let tc = toolchainFor pkgs; in
        lib.optionalAttrs tc.canBuildGuests {
          package = self.packages.${pkgs.stdenv.hostPlatform.system}.default;

          # clang-format over exactly the list cmake/Format.cmake defines, so
          # the check and the `format` target can never disagree.
          format = cmakeDrv pkgs {
            name = "hl-engine-format-check";
            extraNativeBuildInputs = [ pkgs.clang-tools ];
            cmakeFlags = [ "-DHL_BUILD_TESTS=OFF" ];
            # ninjaFlags, not a custom buildPhase: nixpkgs' cmake hook configures
            # into build/ and its buildPhase runs there, whereas an overridden
            # buildPhase does not inherit that cwd and fails to find build.ninja.
            ninjaFlags = [ "format-check" ];
            installPhase = "touch $out";
          };

          # Host-side unit suite via CTest. Guest fixtures auto-disable when the
          # Linux cross compilers are absent (cmake/GuestFixtures.cmake), which
          # is the case in this sandbox, so this stays a pure host-unit check.
          unit = cmakeDrv pkgs {
            name = "hl-engine-unit";
            cmakeFlags = [ "-DHL_BUILD_TESTS=ON" ];
            checkPhase = ''
              runHook preCheck
              ctest -L unit --output-on-failure
              runHook postCheck
            '';
            doCheck = true;
            installPhase = "touch $out";
          };
        } // lib.optionalAttrs tc.canRunGuests {
          rust = self.packages.${pkgs.stdenv.hostPlatform.system}.rust;
        });

      # Only ONE app. Everything that merely verifies is a check (above);
      # everything that produces an artifact is a package. `fmt` is the
      # exception because it REWRITES THE WORKING TREE, which a derivation
      # cannot do -- and it needs a real stdenv for CMake's compiler probe, so
      # it goes through `nix develop`. That nesting is deliberate here, not a
      # pattern to copy.
      apps = forAllSystems (pkgs: {
        fmt = {
          type = "app";
          program = "${pkgs.writeShellApplication {
            name = "hl-fmt";
            runtimeInputs = [ pkgs.nix ];
            text = ''
              export NIX_CONFIG="experimental-features = nix-command flakes"
              # build-fmt/, never build/ -- the latter is the Makefile's tree.
              nix develop --command sh -c '
                cmake -G Ninja -B build-fmt -DHL_BUILD_TESTS=OFF >/dev/null
                ninja -C build-fmt format
              '
            '';
          }}/bin/hl-fmt";
        };
      });

      devShells = forAllSystems (pkgs:
        let tc = toolchainFor pkgs; in
        {
          default = pkgs.mkShell ({
            packages = [
              pkgs.clang
              pkgs.gnumake
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              pkgs.rustc
              pkgs.cargo
              pkgs.rustfmt
              pkgs.clippy
            ] ++ lib.optionals tc.canBuildGuests tc.crossCompilers;

            # Each cc's setup-hook assigns $CC as it runs, so whichever cross
            # compiler happens to be listed last silently won -- which left $CC
            # pointing at the x86_64 cross compiler on every host. That is the
            # "poisoned $CC" the Makefile worked around with CC=cc and the CMake
            # toolchain files dodged by reading *_LINUX_CC explicitly. Pin it
            # back to the native compiler AFTER the hooks have run, so a bare
            # `make` and a plain `cmake -B build` both do the obvious thing.
            shellHook = lib.optionalString tc.canBuildGuests ''
              export CC="${tc.nativeCC}"
              export NATIVE_CC="${tc.nativeCC}"
            '';
          } // lib.optionalAttrs tc.canBuildGuests tc.env);

        });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
