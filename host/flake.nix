{
  description = "MLK-F3P host — 上位机工作区 (DR1M / MLK-F3P-DR1M90G)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { nixpkgs, ... }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));

      # Python 网络 / 绘图环境，通信电脑上位机先用 socket 打点
      hostPy =
        pkgs: pkgs.python3.withPackages (ps: with ps; [
          numpy
          scipy
          matplotlib
          pandas
          pyserial
          pyqt5
        ]);

      # 上位机侧工具
      hostTools =
        pkgs: with pkgs; [
          (hostPy pkgs)

          # 编译 Qt 程序
          gcc
          gnumake
          pkg-config

          # Qt，上位机本地 UI 开发用
          qt5.qtbase
          qt5.qttools
          qt5.qtwayland

          # 网络调试：抓包 / 压测 / 端口转发
          tshark
          iperf3
          socat
          netcat-gnu

          # 故障波形存储加密
          openssl
        ];
    in
    {
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          name = "mlk-f3p-host";

          packages = hostTools pkgs;
          QT_PLUGIN_PATH = pkgs.lib.makeSearchPath "lib/qt-${pkgs.qt5.qtbase.version}/plugins" [
            pkgs.qt5.qtbase.bin
            pkgs.qt5.qtwayland.bin
          ];

          shellHook = ''
            echo "MLK-F3P host 上位机环境  (DR1M / MLK-F3P-DR1M90G)"
            echo "  python : $(python3 --version 2>&1 | awk '{print $2}')   qmake : $(qmake --version 2>/dev/null | tail -1 | awk '{print $NF}')"
            echo "  vendor : ./vendor  (厂商 Qt 运行库 / 驱动 / 手册, gitignored)"
            echo
            echo "提示: PS 端本地 Qt 的交叉工具链用 vendor/qt 的厂商 SDK，不在此 flake 内。"
            echo "用法: cd host && nix develop"
          '';
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt-rfc-style);
    };
}
