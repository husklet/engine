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
            version = "0.1.10";
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

            doCheck = true;
            checkPhase = ''
              runHook preCheck
              make unit
              make package-test ${pkgs.lib.optionalString (system == "aarch64-darwin") "CODESIGN=:"}
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
        } // pkgs.lib.optionalAttrs runtimeHost {
          rust = pkgs.rustPlatform.buildRustPackage {
            pname = "hl-engine";
            version = "0.1.10";
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
          linuxArmCompiler = "${linuxArm.stdenv.cc}/bin/${linuxArm.stdenv.cc.targetPrefix}cc";
          linuxX86Compiler = "${linuxX86.stdenv.cc}/bin/${linuxX86.stdenv.cc.targetPrefix}cc";
        in {
          default = pkgs.mkShell ({
            packages = [
              pkgs.clang
              pkgs.gnumake
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
            X86_64_LINUX_STATIC_CC = "${linuxX86Compiler} -L${linuxX86.glibc.static}/lib";
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
            X86_64_LINUX_STATIC_CC = "${linuxX86Compiler} -L${linuxX86.glibc.static}/lib";
            AARCH64_DYNAMIC_LOADER = "${pkgs.glibc}/lib/ld-linux-aarch64.so.1";
            AARCH64_DYNAMIC_LIBC = "${pkgs.glibc}/lib/libc.so.6";
            X86_64_DYNAMIC_LOADER = "${linuxX86.glibc}/lib/ld-linux-x86-64.so.2";
            X86_64_DYNAMIC_LIBC = "${linuxX86.glibc}/lib/libc.so.6";
          });
        });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
