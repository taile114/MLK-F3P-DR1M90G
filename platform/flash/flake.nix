{
  description = "MLK-F3P platform/flash — 构建与烧录工作区 (DR1M / MLK-F3P-DR1M90G)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));

      # 编译「内核 / U-Boot / Buildroot」所需的宿主机（host）工具
      hostTools =
        pkgs: with pkgs; [
          # 基础编译与文本工具
          gnumake
          gcc
          binutils
          patch
          diffutils
          findutils
          gnugrep
          gnused
          gawk
          coreutils
          m4
          file
          which

          # 内核 / U-Boot / 设备树
          flex
          bison
          bc
          pkg-config
          ubootTools # mkimage
          dtc # device-tree-compiler
          kmod
          cpio
          elfutils # libelf（objtool）
          zlib
          ncurses # menuconfig
          openssl

          # 压缩
          lz4
          xz
          gzip
          bzip2
          zstd
          lzip

          # 网络 / 同步 / 版本控制
          rsync
          wget
          curl
          git

          # 脚本与归档
          python3
          perl
          texinfo
          unzip
          zip

          # 分区 / 文件系统 / 镜像
          parted
          e2fsprogs
          dosfstools
          mtools
          util-linux

          # autotools（Buildroot 的 host 包常用）
          autoconf
          automake
          libtool
          gettext
        ];
    in
    {
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          name = "mlk-f3p-flash";

          packages = hostTools pkgs;

          shellHook = ''
            echo "MLK-F3P platform/flash 构建/烧录环境  (DR1M / MLK-F3P-DR1M90G)"
            echo "  vendor : ./vendor  (解压的 uisrc-lab-anlogic, gitignored)"
            echo "  images : ./vendor/boards/DR1M/buildroot/images"
            echo
            echo "用法："
            echo "  ./apply_sdk_local.sh          # 给 vendor 打本地补丁 (NixOS file 检查 / vim)"
            echo "  ./build_rootfs.sh             # 编译 rootfs"
            echo "  sudo env PATH=\"\$PATH\" ./burn_sd.sh /dev/sda   # 烧录"
          '';
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt-rfc-style);
    };
}
