# Windows 11 SDR 亮度

一款常驻托盘的 Windows 11 小工具：直接调节 HDR 显示器的 **SDR 内容亮度**，不用再翻系统设置里的多层菜单。

> 本项目由 **DeepSeek** 制作。

## 功能

- 托盘常驻，左键弹出亮度面板，拖动滑块或滚动滚轮即可调节（0–100%）
- 通过 DisplayConfig 直接写入显示器 SDR white level，效果与系统「SDR 内容亮度」设置完全一致
- 多显示器支持：分别调节，或一键同步全部显示器
- 全局快捷键：`Ctrl+Alt+B` 显示/隐藏面板，`Ctrl+Alt+↑` / `Ctrl+Alt+↓` 以 5% 步进调节
- 跟随系统深/浅色主题，DWM 亚克力材质、圆角窗口与开合动画，支持高 DPI
- 开机自启、单实例运行
- 纯 Win32 + C++20 编写，静态链接，单个 exe 无运行时依赖

## 使用

1. 下载 [bin/SdrTray.exe](bin/SdrTray.exe)，或前往 [Releases](../../releases) 下载最新版
2. 双击运行，托盘出现图标
3. 左键图标打开面板并拖动滑块；右键图标打开菜单（同步所有显示器 / 开机时启动 / 打开显示设置 / 退出）

面板只列出已开启 HDR 的显示器 —— SDR 内容亮度是 HDR 模式下的概念，HDR 关闭时没有可调项。

## 从源码构建

需要 Visual Studio 的 MSVC C++ 生成工具与 Windows SDK（x64）。脚本会自动定位工具链，无需手动打开开发者命令行：

```powershell
.\build.ps1
```

产物为 `bin\SdrTray.exe`。加 `-Clean` 可先清理 `obj` 与 `bin` 目录。

## 环境要求

- Windows 11 64 位（Windows 10 未验证）
- 显示器需开启 HDR
