{
  description = "hl-engine: Linux execution engine and language bindings";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { self, nixpkgs }:
    let
      systems = [ "aarch64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = function:
        nixpkgs.lib.genAttrs systems (system: function (import nixpkgs {
          inherit system;
          config.allowUnsupportedSystem = true;
        }));
    in {
      packages = forAllSystems (pkgs:
        let
          system = pkgs.stdenv.hostPlatform.system;
          runtimeHost = system == "aarch64-darwin" || system == "aarch64-linux";
          linuxArm = pkgs.pkgsCross.aarch64-multiplatform;
          linuxX86 = pkgs.pkgsCross.gnu64;
          linuxArmCompiler = "${linuxArm.stdenv.cc}/bin/${linuxArm.stdenv.cc.targetPrefix}cc";
          linuxX86Compiler = "${linuxX86.stdenv.cc}/bin/${linuxX86.stdenv.cc.targetPrefix}cc";
        in {
          default = pkgs.stdenv.mkDerivation {
            pname = "hl-engine";
            version = "0.1.25";
            src = pkgs.lib.cleanSource self;
            strictDeps = true;
            nativeBuildInputs = [ pkgs.gnumake pkgs.pkg-config ]
              ++ pkgs.lib.optionals (system == "aarch64-darwin") [
                pkgs.darwin.cctools
                linuxArm.stdenv.cc
                linuxX86.stdenv.cc
              ]
              ++ pkgs.lib.optionals (system == "aarch64-linux") [ linuxX86.stdenv.cc ];
            AARCH64_LINUX_CC = if system == "aarch64-darwin" then linuxArmCompiler else "${pkgs.stdenv.cc}/bin/cc";
            AARCH64_LINUX_STATIC_CC = if system == "aarch64-darwin"
              then "${linuxArmCompiler} -L${linuxArm.glibc.static}/lib"
              else "${pkgs.stdenv.cc}/bin/cc -L${pkgs.glibc.static}/lib";
            X86_64_LINUX_CC = linuxX86Compiler;
            X86_64_LINUX_STATIC_CC = "${linuxX86Compiler} -L${linuxX86.glibc.static}/lib";
            enableParallelBuilding = true;

            buildPhase = ''
              runHook preBuild
              make all
              ${pkgs.lib.optionalString (system == "aarch64-darwin") ''
                make MAC= CODESIGN=/usr/bin/codesign build/production/hl-engine-linux-aarch64 \
                  build/production/hl-engine-linux-x86_64
              ''}
              ${pkgs.lib.optionalString (system == "aarch64-linux") ''
                make build/linux-production/hl-engine-linux-aarch64 \
                  build/linux-production/hl-engine-linux-x86_64
              ''}
              runHook postBuild
            '';

            # The runtime test suites (`make unit`, `make package-test`) run the engine
            # and its host-service tests, which need resources the Nix darwin build
            # sandbox denies under `(deny default)`: opening "/", scratch files under a
            # fixed /tmp path, JIT execution of guest binaries, and localhost DNS. On
            # macos-26 the sandbox is active and these fail. Rather than widen the build
            # sandbox for genuine runtime behaviour, darwin builds+installs only here and
            # runs `make unit` + `make package-test` as unsandboxed steps in mac.yml
            # (mirroring how test-macos and the compat suite already run there, and how
            # Linux runs its compat suite outside the sandbox). Linux keeps the full
            # in-sandbox check, which passes.
            doCheck = system != "aarch64-darwin";
            checkPhase = ''
              runHook preCheck
              make unit
              make package-test
              runHook postCheck
            '';

            installPhase = ''
              runHook preInstall
              make install DESTDIR="$out" PREFIX=
              mkdir -p "$out/bin"
              ${pkgs.lib.optionalString (system == "aarch64-darwin") ''
                install -m755 build/production/hl-engine-linux-aarch64 "$out/bin/"
                install -m755 build/production/hl-engine-linux-x86_64 "$out/bin/"
              ''}
              ${pkgs.lib.optionalString (system == "aarch64-linux") ''
                install -m755 build/linux-production/hl-engine-linux-aarch64 "$out/bin/"
                install -m755 build/linux-production/hl-engine-linux-x86_64 "$out/bin/"
              ''}
              runHook postInstall
            '';

            passthru = { inherit runtimeHost; };
            meta = {
              description = "Userspace execution engine for Linux programs";
              homepage = "https://github.com/husklet/engine";
              license = pkgs.lib.licenses.mit;
              platforms = systems;
              mainProgram = "hl-engine-runner";
            };
          };
        } // {
          # ---------------------------------------------------------------
          # CMake-driven build: `nix build .#cmake`
          #
          # The goal is for nix to drive CMake and for CMake to own the build
          # graph, so there is one standard way to build this project. This
          # package is the migration target for `default` (which still shells
          # out to make while CI's authoritative gate does).
          #
          # No toolchain file is passed: inside a nix build the stdenv compiler
          # IS the intended host compiler, and cmake/toolchains/* exist for the
          # devShell case where $CC is deliberately poisoned to the x86_64 cross
          # compiler. Guest-fixture / compat targets need the cross toolchains
          # and are not part of this derivation; it builds the libraries, the
          # production engines and the runner, then installs the SDK.
          # ---------------------------------------------------------------
          cmake = pkgs.stdenv.mkDerivation {
            pname = "hl-engine-cmake";
            version = "0.1.25";
            src = pkgs.lib.cleanSource self;
            strictDeps = true;
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
            # CMAKE_BUILD_TYPE stays empty so cmake injects no -O/-DNDEBUG of
            # its own: the flag sets are specified exactly in CMakeLists.txt.
            cmakeBuildType = "";
            ninjaFlags = [ "hl-engine" "hl-translator" "hl-linux-abi" "hl-host-fake" ]
              ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux
                   [ "hl-host-linux" "hl-engine-runner" ];
            doCheck = false;
            meta = {
              description = "hl-engine built through CMake (nix drives cmake)";
              license = pkgs.lib.licenses.mit;
              platforms = systems;
            };
          };
        } // pkgs.lib.optionalAttrs runtimeHost {
          rust = pkgs.rustPlatform.buildRustPackage {
            pname = "hl-engine";
            version = "0.1.25";
            # The publishable `hl-engine` crate lives at pkgs/rust; its former
            # api/provider/protocol/runtime member crates are now internal
            # modules, so the whole pkgs/rust tree is the sandbox.
            src = pkgs.lib.cleanSourceWith {
              src = ./pkgs/rust;
              filter = path: type:
                let base = baseNameOf (toString path);
                in !(type == "directory" && base == "target");
            };
            cargoLock.lockFile = ./pkgs/rust/Cargo.lock;
            nativeBuildInputs = [ pkgs.pkg-config ];
            # Build the crate (lib + bins). Native-linking test binaries are
            # skipped: they link the frozen `libhl-engine.a` activation archive,
            # whose refresh is a separately gated frozen-asset publication (see
            # pkgs/rust/assets/PROVENANCE.md), not part of the structural build
            # this check guards.
            doCheck = false;
          };
        });

      # ---------------------------------------------------------------------
      # Task runners: `nix run .#<task>`. These exist so there is ONE obvious
      # way to do each common thing, without memorising cmake invocations.
      #
      # `fmt` and `fmt-check` are self-contained. `build` and `test` need the
      # Linux cross compilers for the guest test corpus, so they run inside the
      # devShell environment (`nix develop -c ...`) rather than duplicating the
      # toolchain wiring here -- the devShell is already the single place those
      # variables are defined.
      # ---------------------------------------------------------------------
      apps = forAllSystems (pkgs:
        let
          system = pkgs.stdenv.hostPlatform.system;
          # These wrappers shell back into `nix develop`, and that nested call
          # inherits none of the outer invocation's CLI flags -- without
          # NIX_CONFIG it dies with "experimental Nix feature 'nix-command' is
          # disabled". Set it once here rather than in every task body.
          mkApp = name: body: {
            type = "app";
            program = "${pkgs.writeShellApplication {
              inherit name;
              runtimeInputs = [ pkgs.cmake pkgs.ninja pkgs.clang-tools pkgs.nix ];
              text = ''
                export NIX_CONFIG="experimental-features = nix-command flakes"
              '' + body;
            }}/bin/${name}";
          };
          toolchain = "cmake/toolchains/aarch64-linux.cmake";
        in {
          # nix run .#fmt          -- clang-format the tree in place
          # Runs inside the devShell like the others: the toolchain files read
          # $AARCH64_LINUX_CC, which only exists there, and outside it CMake
          # fails with an unset CMAKE_C_COMPILER even though formatting needs
          # no compiler at all.
          fmt = mkApp "hl-fmt" ''
            nix develop --command sh -c '
              cmake -G Ninja -B build-fmt -DCMAKE_TOOLCHAIN_FILE=${toolchain} -DHL_BUILD_TESTS=OFF >/dev/null
              ninja -C build-fmt format
            '
          '';
          # nix run .#fmt-check    -- fail if anything is unformatted
          fmt-check = mkApp "hl-fmt-check" ''
            nix develop --command sh -c '
              cmake -G Ninja -B build-fmt -DCMAKE_TOOLCHAIN_FILE=${toolchain} -DHL_BUILD_TESTS=OFF >/dev/null
              ninja -C build-fmt format-check
            '
          '';
          # nix run .#build        -- configure + build everything
          build = mkApp "hl-build" ''
            nix develop --command sh -c '
              cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=${toolchain}
              ninja -C build
            '
          '';
          # nix run .#test         -- build + run the unit suite via CTest
          test = mkApp "hl-test" ''
            nix develop --command sh -c '
              cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=${toolchain}
              ninja -C build
              ctest --test-dir build -L unit --output-on-failure
            '
          '';
        });

      checks = forAllSystems (pkgs:
        let system = pkgs.stdenv.hostPlatform.system;
        in {
          package = self.packages.${system}.default;
        } // pkgs.lib.optionalAttrs (system == "aarch64-darwin" || system == "aarch64-linux") {
          rust = self.packages.${system}.rust;
        });

      devShells = forAllSystems (pkgs:
        let
          system = pkgs.stdenv.hostPlatform.system;
          linuxArm = pkgs.pkgsCross.aarch64-multiplatform;
          linuxX86 = pkgs.pkgsCross.gnu64;
          linuxArmSqlite = linuxArm.pkgsStatic.sqlite;
          linuxX86Sqlite = linuxX86.pkgsStatic.sqlite;
          linuxArmCompiler = "${linuxArm.stdenv.cc}/bin/${linuxArm.stdenv.cc.targetPrefix}cc";
          linuxX86Compiler = "${linuxX86.stdenv.cc}/bin/${linuxX86.stdenv.cc.targetPrefix}cc";
        in {
          default = pkgs.mkShell ({
            packages = [
              pkgs.clang
              pkgs.gnumake
              # Phase-1 CMake build (additive, non-gating; see CMakeLists.txt and
              # cmake/toolchains/*.cmake, which read the same *_LINUX_CC vars this shell exports).
              # `make` remains the authoritative build; these are dev tools only.
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              pkgs.rustc
              pkgs.cargo
              pkgs.rustfmt
              pkgs.clippy
            ] ++ pkgs.lib.optionals (system == "aarch64-darwin") [
              linuxArm.stdenv.cc linuxX86.stdenv.cc
            ] ++ pkgs.lib.optionals (system == "aarch64-linux") [
              pkgs.gcc linuxX86.stdenv.cc
            ];
          } // pkgs.lib.optionalAttrs (system == "aarch64-darwin") {
            CC = "${pkgs.stdenv.cc}/bin/cc";
            NATIVE_CC = "${pkgs.stdenv.cc}/bin/cc";
            AARCH64_LINUX_CC = linuxArmCompiler;
            X86_64_LINUX_CC = linuxX86Compiler;
            AARCH64_LINUX_STATIC_CC = "${linuxArmCompiler} -I${nixpkgs.lib.getDev linuxArmSqlite}/include -L${nixpkgs.lib.getLib linuxArmSqlite}/lib -L${linuxArm.glibc.static}/lib";
            X86_64_LINUX_STATIC_CC = "${linuxX86Compiler} -I${nixpkgs.lib.getDev linuxX86Sqlite}/include -L${nixpkgs.lib.getLib linuxX86Sqlite}/lib -L${linuxX86.glibc.static}/lib";
            AARCH64_DYNAMIC_LOADER = "${linuxArm.glibc}/lib/ld-linux-aarch64.so.1";
            AARCH64_DYNAMIC_LIBC = "${linuxArm.glibc}/lib/libc.so.6";
            X86_64_DYNAMIC_LOADER = "${linuxX86.glibc}/lib/ld-linux-x86-64.so.2";
            X86_64_DYNAMIC_LIBC = "${linuxX86.glibc}/lib/libc.so.6";
          } // pkgs.lib.optionalAttrs (system == "aarch64-linux") {
            CC = "${pkgs.stdenv.cc}/bin/cc";
            NATIVE_CC = "${pkgs.stdenv.cc}/bin/cc";
            AARCH64_LINUX_CC = "${pkgs.stdenv.cc}/bin/cc";
            X86_64_LINUX_CC = linuxX86Compiler;
            AARCH64_LINUX_STATIC_CC = "${pkgs.stdenv.cc}/bin/cc -I${nixpkgs.lib.getDev pkgs.pkgsStatic.sqlite}/include -L${nixpkgs.lib.getLib pkgs.pkgsStatic.sqlite}/lib -L${pkgs.glibc.static}/lib";
            X86_64_LINUX_STATIC_CC = "${linuxX86Compiler} -I${nixpkgs.lib.getDev linuxX86Sqlite}/include -L${nixpkgs.lib.getLib linuxX86Sqlite}/lib -L${linuxX86.glibc.static}/lib";
            AARCH64_DYNAMIC_LOADER = "${pkgs.glibc}/lib/ld-linux-aarch64.so.1";
            AARCH64_DYNAMIC_LIBC = "${pkgs.glibc}/lib/libc.so.6";
            X86_64_DYNAMIC_LOADER = "${linuxX86.glibc}/lib/ld-linux-x86-64.so.2";
            X86_64_DYNAMIC_LIBC = "${linuxX86.glibc}/lib/libc.so.6";
          });
        });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
