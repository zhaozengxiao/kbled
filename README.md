# kbled — Clevo 笔记本键盘背光 Linux 控制工具

在 Linux 下控制神舟/蓝天 (HASEE / Clevo NH5x/NH7x 系列) 笔记本的键盘背光。
无需任何内核驱动，直接通过 ACPI EC 命令邮箱下发指令。

> **实测机型**：HASEE NH5x_NH7xHP（神舟战神系列模具），Ubuntu 26.04 LTS，内核 7.0。
> 同属 Clevo-ODM 模具 + Insyde EC 固件的机器（如 TUXEDO、Monster、XMG 对应型号）理论上通用。

## 为什么需要它

- 此系列键盘背光的 **Fn 快捷键在 Linux 下无效**——EC 固件把热键事件转发给
  Windows 的 OS 驱动（ACPI 设备 `DCHU` / `HKDR`），Linux 没有对应驱动。
- 内核没有此平台的背光驱动（`clevo-wmi` 等均不适用）。
- 直接改写内存镜像邮箱（`0xFE0B0380` 区域）是无效的：值能写入，LED 不理会
  （开机后 EC 静默忽略，必须通过命令邮箱下发「主使能→颜色→亮度」序列）。

## 功能

- 开 / 关
- 颜色（RGB，单区整键盘）
- 亮度 0–255
- 开机自启 + 唤醒后自动恢复上次设置（systemd）

## 安装

```bash
git clone https://github.com/zhaozengxiao/kbled
cd kbled
sudo ./install.sh
```

`install.sh` 会：

1. 编译 `kbled.c` 并安装到 `/usr/local/sbin/kbled`
2. 写入默认配置 `/etc/kbled.conf`（白色、亮度 200、开启）
3. 安装并启用 `kbled.service`（开机 + 睡眠/休眠唤醒后自动应用配置）
4. 添加 sudoers 规则，使当前用户**免密**运行 `sudo kbled`

## 用法（免密 sudo）

```bash
sudo kbled on                # 开灯
sudo kbled off               # 关灯
sudo kbled color 00C8FF      # 蓝青色
sudo kbled color FF0000      # 红色
sudo kbled color FFFFFF      # 白色
sudo kbled brightness 150    # 亮度 0-255
sudo kbled brightness 0      # 亮度 0 相当于关
sudo kbled status            # 查看当前配置与 EC 状态
sudo kbled apply             # 应用配置文件（服务在开机/唤醒时调用）
```

改完颜色/亮度自动保存，开机会自动恢复。

## EC 协议（逆向记录）

键盘背光通过 EC 命令邮箱（DSDT 中 `ECMD` 定义，EC 偏移 `0xF8`–`0xFD`）控制：

| 偏移 | 名称 | 作用 |
|------|------|------|
| `0xF8` | FCMD | 命令门铃 —— **最后写**，触发执行 |
| `0xF9` | FDAT | 子命令 / 参数 1 |
| `0xFA` | FBUF | 参数 2 |
| `0xFB` | FBF1 | 参数 3 |
| `0xFC` | FBF2 | 参数 4 |
| `0xFD` | FBF3 | 参数 5 |

**注意**：先写参数、最后写 `0xF8`；EC 执行后会把 `FCMD` 清零。

### 命令表

| 操作 | FDAT | FBUF | FBF1 | FBF2 | FCMD |
|------|------|------|------|------|------|
| 键盘状态主使能（**开机后必须先发**） | `0x0C` | `0x3F` | – | – | `0xC4` |
| 键盘状态主禁用 | `0x0C` | `0x20` | – | – | `0xC4` |
| 设置整键盘颜色 | `0x03` | **B** | **R** | **G** | `0xCA` |
| 设置亮度 0–255 | `0x06` | 亮度 | – | – | `0xCA` |

- **颜色通道顺序是 B,R,G**（不是 R,G,B）——这是 Clevo 固件的老传统。
- 分区 0/1/2 对应 FDAT `0x03/0x04/0x05`；本机三区并联（单区键盘），整键盘一色。
- 协议逆向来源：[smairio/gigactl](https://github.com/smairio/gigactl) 的
  [PROTOCOL.md](https://github.com/smairio/gigactl/blob/main/docs/PROTOCOL.md)
  （Gigabyte G5/G6，同为 Clevo-ODM + Insyde EC），已在本文档所述的机器上逐项复测验证。

## 常见问题

**Q: 改了颜色但没反应？**
先确认不是刚开机且未运行 `sudo kbled on` / 服务未生效——开机后 EC 会忽略背光命令，
必须收到过一次「主使能」（`0xC4 / FDAT=0x0C / FBUF=0x3F`）才接受后续命令。

**Q: 直接改 `/sys/kernel/debug/ec/ec0/io` 里 `0xFE0B0380` 区域为什么没用？**
那是 EC 的状态镜像邮箱，值为只读回显，不驱动 LED。读写必须以命令邮箱下发。

**Q: 需要 root 吗？**
是的（EC 访问需要权限）。`install.sh` 已为本机用户配置免密 sudo。
其他用户可自行执行 `sudo kbled ...`。

## 免责声明

EC 命令作用于固件层面，请在理解风险的前提下使用。本项目与蓝天电脑/Clevo 无任何关联，
品牌名仅用于标识兼容硬件。

## 许可证

MIT