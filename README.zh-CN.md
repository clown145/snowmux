# SnowMux

[English](./README.md) | **简体中文**

**SnowMux** 是面向 **雪酥·3**（板型 `xuesu-300-std`）的社区阅读固件，基于 [CrossMux](https://github.com/0x1abin/crossmux)。它适配了雪酥的 300×400 墨水屏、三个实体按键和原厂分区布局。

[雪酥网页刷机](https://clown145.github.io/snowmux/) · [SnowMux 固件发布](https://github.com/clown145/snowmux/releases) · [设备与构建指南](./docs/engineering/xuesu-300-port.md)

## 安装与更新

1. 备份 SD 卡中的书籍和数据。用电脑上的 Chrome 或 Edge 打开[雪酥网页刷机](https://clown145.github.io/snowmux/)，用支持数据传输的 USB 线连接设备，按确认键唤醒。
2. 按网页提示连接串口、检查设备。首次安装前，先在网页下载并保留本机的 16 MiB Flash 备份，再安装 SnowMux。
3. 已安装带在线更新功能的 SnowMux 后，可在「设置 → 系统 → 检查更新」联网更新。旧版没有这个入口时，使用网页刷机。

首次通过网页安装会保留原厂应用槽。如果所用版本支持设备内更新，请留意确认提示：更新可能覆盖原厂应用所在槽位。若菜单提供「恢复原厂固件」，可联网安装原厂应用；需要恢复整片 Flash 时使用首次保存的本机备份。详见[网页刷机与发布说明](./docs/engineering/xuesu-300-port.md#社区刷机站与发版)。

## 三键与快捷操作

| 操作 | 用途 |
| --- | --- |
| 上键、下键 | 在列表移动；阅读时翻页。 |
| 短按确认 | 打开选中项；在正文中打开阅读菜单；休眠后唤醒。 |
| 长按确认 | 通常返回上一层或取消。在带标签栏的主页面可返回标签栏，用上下键换标签。 |
| 双击确认 | 在主标签页或阅读正文打开悬浮快捷栏。上下选择，短按执行，长按关闭。 |

快捷栏提供休眠、夜间模式、刷新屏幕和阅读方向；在文件、书库、书签、剪藏、OPDS 分页目录及待机显示等页面，还可操作当前条目。

首次使用新 SD 卡时，首页会自动添加《SnowMux 使用说明书》；简体中文界面使用中文版，其他界面语言使用英文版。已用过的卡升级后不会自动替换旧手册。

## 阅读与传书

- **阅读和书库**：支持 EPUB、TXT、XTC/XTCH，以及无 DRM 且源文件不超过 4 MiB 的 MOBI/AZW3。DOCX 会重排正文与支持的内嵌图片。支持书签、剪藏、离线词典、SD 卡字体和 KOReader 进度同步。原厂系统的最近阅读可从「设置 → 系统 → 导入原厂最近阅读」导入。
- **USB 与无线传书**：进入「应用 → 文件传输」。选「USB 磁盘」可把 SD 卡交给电脑，复制完先在电脑安全弹出；也可使用 Wi-Fi 网页传书或 Calibre 无线。文件页看到新书但书库还没收录时，从悬浮快捷栏进入「书库操作 → 刷新书库」。
- **用手机配网和输入**：进入「设置 → 连接 → 热点配网」，手机扫码加入临时热点，在网页选择并填写 2.4 GHz Wi-Fi。配置 OPDS、KOReader 账号与地址、输入目录搜索词等也可按屏幕提示在手机网页完成。
- **翻页器、统计与表盘**：在「设置 → 连接 → 蓝牙翻页器」配对 BLE HID 翻页器。设备提供阅读统计和热力图。进入「应用 → 待机显示」预览表盘，双击确认可把当前表盘设为休眠画面。

书籍、设置和进度建议随 SD 卡一起备份。`/.crosspoint/` 中有阅读进度和设置，清理单本书时不要删除整个目录。

## 应用与可见性

下表对应当前源码的新设置。以前保存过的显示选项会保留。

| 状态 | 雪酥·3 的应用 |
| --- | --- |
| 默认显示 | 书库、文件传输、微信读书、AirPage、阅读统计、待机显示。 |
| 配置后显示 | OPDS 浏览器需要先在「设置 → 连接 → OPDS 服务器」添加服务器。 |
| 默认隐藏，可打开 | 丑丑头像、Buddy、电子木鱼。 |
| 雪酥菜单不提供 | 数独、五子棋、推箱子、中国象棋、扫雷、2048、像素大战、计算器。这些应用目前依赖触屏或独立左右键，雪酥版的「隐藏应用」里也没有对应开关。 |

在「设置 → 系统 → 隐藏应用」中，选中应用并短按确认切换显示状态；显示为「开」即在应用菜单显示，长按确认退出并保存。微信读书的入口不受界面语言限制，使用方式见[微信读书说明](./src/activities/apps/weread/README.md)。

## 文档与构建

- [雪酥设备指南](./docs/engineering/xuesu-300-port.md)：设备限制、源码构建、发布流程及真机验收记录。
- [应用开发](./src/activities/apps/README.md) · [Web 传书](./docs/webserver.md) · [Web API](./docs/webserver-endpoints.md) · [SD 卡字体](./docs/sd-card-fonts.md)
- [内置说明书源文件](./docs/user-guide/xuesu/) · [工程文档](./docs/engineering/index.md) · [贡献指南](./docs/contributing/README.md)

递归克隆 [SnowMux 仓库](https://github.com/clown145/snowmux)，安装 pioarduino Core 和 Python，并按[源码构建步骤](./docs/engineering/xuesu-300-port.md#从源码构建)应用 FreeInk SDK 补丁、生成屏幕波形头文件。准备完成后，在 Bash 中运行：

```bash
PYTHONUTF8=1 pio run -e xuesu_300
```

应用镜像位于 `.pio/build/xuesu_300/firmware.bin`。安装时请使用雪酥刷机流程；PlatformIO 生成的 `firmware.factory.bin` 不能用于雪酥首次安装。

其他设备的说明请参考[上游 CrossMux](https://github.com/0x1abin/crossmux)、[通用用户指南](./USER_GUIDE.md)和[设备变体说明](./docs/engineering/device-variants.md)。CrossMux Stable 和 Nightly 的固件不适用于雪酥·3。

## 致谢

感谢 [CrossMux](https://github.com/0x1abin/crossmux)、[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)、[Inx](https://github.com/obijuankenobiii/inx)、[cpr-vcodex](https://github.com/franssjz/cpr-vcodex) 及其贡献者，以及带来最初启发的 [diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader)。

SnowMux 是独立社区项目。仓库许可证见 [LICENSE](./LICENSE)，固件依赖与二进制发布许可见[固件许可说明](./licenses/README.md)。
