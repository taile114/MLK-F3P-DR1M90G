# platform — 平台工作区

MLK-F3P-DR1M90G 的平台工作区，与 `docs/` 同级，分三块：

- **flash/** — Linux/Buildroot 构建与 SD 烧录，日常开发都在这。见 [`flash/README_CN.md`](flash/README_CN.md)。
- **baremetal-sdk/** — 官方 SDK（app / fsbl / linux / openssl 等），编 FSBL、裸机应用用。
- **factory-image/** — 原厂镜像，恢复出厂系统用。

`baremetal-sdk/`、`factory-image/` 是厂商资料，未入库，各自目录里带一个 `.tar.gz`。
