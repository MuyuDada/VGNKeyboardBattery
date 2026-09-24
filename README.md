# VGN 键盘电量插件（for TrafficMonitor）

在 **TrafficMonitor** 的任务栏 / 主窗口中显示 **VGN V98Pro** 系列机械键盘的剩余电量。

> 插件遵循 TrafficMonitor 的[插件开发指南](https://github.com/zhongyang219/TrafficMonitor/wiki/%E6%8F%92%E4%BB%B6%E5%BC%80%E5%8F%91%E6%8C%87%E5%8D%97)，
> 是一个标准的 C++ 动态库（DLL），把 DLL 丢进 `plugins` 目录即可被主程序自动加载。

---

## 一、效果

| 位置 | 显示内容 |
|------|----------|
| 任务栏 / 主窗口 | `键盘 85%`（标签 + 数值），并带一条表示剩余电量的柱状图 |
| 鼠标悬停提示 | `VGN V98Pro  电量：85%  电压：3980 mV` |
| 右键菜单 → 插件命令 | `立即刷新键盘电量` |

无线模式下键盘休眠时显示 `--`，插件会每 1 秒尝试唤醒一次；唤醒后自动恢复显示。

---

## 二、支持的型号

| 型号 | VID | PID | 协议 | Report ID | 状态 |
|------|-----|-----|------|-----------|------|
| VGN V98Pro（原版 / V2）有线 | `0x320F` | `0x5055` | 维盛(weisheng) | 4（Output） | ✅ 已实测通过 |
| VGN V98Pro（原版 / V2）2.4G 接收器 | `0x320F` | `0x5088` | 维盛(weisheng) | 4（Output） | 协议同有线 |
| VGN V98Pro V3 有线 | `0x258A` | `0x024E` | 北鹰(beiying) | 9（Feature） | 按官方协议实现 |
| VGN V98Pro V3 2.4G 接收器 | `0x258A` | `0x024F` | 北鹰(beiying) | 19（Output） | 按官方协议实现 |

> “已实测通过”是指在接有该键盘的机器上跑 `test/PluginTester.exe`，
> 成功读到真实电量（`100%`、充电中）。

> VGN V98Pro V4 采用的是 VIA 协议（Usage Page `0xFF60` / Usage `0x61`），
> 指令体系与上面完全不同，本插件暂不支持。若你的键盘是 V4 版，可以告诉我，我再补上。

---

## 三、协议是怎么来的（逆向记录）

VGN 官方网页驱动 **VGN HUB**（<https://hub.vgnlab.com.cn/>）本身就是一个 WebHID 应用，
它直接通过浏览器的 HID 接口和键盘通信。把它的前端 JS 拉下来分析，就能拿到**官方自己使用的**完整协议。

分析入口（`index-*.js` 中的设备表）：

```js
{ vendorId: 12815 /*0x320F*/, productId: 20565 /*0x5055*/,
  productName: "VGN V98pro", customDeviceType: "weisheng",
  customDeviceController: "deviceKeyboard", reportId: 4 }
```

### 协议 A：维盛方案（V98Pro 原版 / V2）

`customDeviceType == "weisheng"` 时使用 `WHe` 控制器：

```js
// 查询电量
async getElectricQuantity() {
  let e = new Uint8Array([0, 0, Oo.GetBatterLevel /* = 26 = 0x1A */, 6, 0, 0, 0]);
  return await this.sendBuffer(e), true;
}

// 发送：直接写 Output Report，无校验字节
async sendBuffer(e) { await this.device.sendReport(this.reportId /* 4 */, e); }

// 接收：解析 Input Report
readBuffer() {
  this.device.oninputreport = async ev => {
    if (ev.reportId !== this.reportId) return;
    let i = new Uint8Array(ev.data.buffer), o = i.slice(7);
    if (i[2] == 26) {                       // 0x1A = 电量应答
      this.deviceInfo.battery.level    = o[0];   // == i[7] 电量百分比
      this.deviceInfo.battery.charging = o[1];   // == i[8] 充电标志
    }
  };
}
```

**落到原生 HID 上：**

```
发送：Output Report，Report ID = 4
      payload = 00 00 1A 06 00 00 00（不足 64 字节补 0）

接收：Input Report，Report ID = 4
      buf[0] = 4（Report ID）
      buf[3] == 0x1A           → 这是电量应答
      buf[8]                   → 电量百分比
      buf[9]                   → 1 = 充电中
```

> ⚠️ **一个很关键的坑：必须用 `WriteFile` 发送，不能用 `HidD_SetOutputReport`。**
>
> 该键盘的厂商通道只在**中断 OUT 端点**上接收指令：
> `HidD_SetOutputReport()` 走的是 `IOCTL_HID_SET_OUTPUT_REPORT`（控制传输），
> 调用会**返回成功**，但设备完全不理会，于是表现为“命令发出去了却永远收不到回包”。
> 换成 `WriteFile()`（走中断 OUT 传输）后立刻就能收到应答。
> 这个坑在 WebHID 里不存在，因为浏览器底层本来就用的中断传输。

### 实测记录

在接有 VGN V98Pro（USB 有线，VID `0x320F` / PID `0x5055`）的机器上抓到的真实报文：

```
发送（WriteFile，64 字节）：04 00 00 1A 06 00 00 00 00 ... 00
接收（ReadFile，64 字节）：04 00 00 1A 06 00 00 00 64 01 00 00 ... 00
                                              ↑↑ ↑↑
                                    电量 = 0x64 = 100%   充电 = 1
```

`HidProbe.exe` 就是这个实验用的探针程序，可以自己跑一遍复现。

### 协议 B：北鹰方案（V98Pro V3 有线）

`customDeviceType == "beiying"`、VID/PID 为 `0x258A:0x024E` 时使用 `lWe` 控制器：

```js
async sendCmd(e, n = 0) {
  let i = new Uint8Array(e.length + 1);
  i.set(e, 0);
  while (i.length < 520) i = new Uint8Array([...i, 0]);   // 补齐到 520 字节
  return this.sendHidBuffer(i, n);
}
async sendHidBuffer(e, n, useFeature = true) {
  if (useFeature) {
    await this.device.sendFeatureReport(this.reportId /* 9 */, e);
    if (n) { await delay(20); return await this.device.receiveFeatureReport(this.reportId); }
  } else {
    await this.device.sendReport(this.reportId, e);
  }
}
async getBattery() {
  const r = await this.sendCmd(new Uint8Array([135, 0, 0, 1, 0, 2, 0]), 1);
  this.deviceInfo.battery.level    = r.getUint8(8);
  this.deviceInfo.battery.charging = (r.getUint8(9) >> 4 & 0xF) === 1;
}
```

```
发送：Feature Report，Report ID = 9
      payload = 87 00 00 01 00 02 00（补齐到 520 字节）
回读：Feature Report 9
      buf[0] = 9（Report ID）
      buf[9]  → 电量百分比
      buf[10] >> 4 == 1 → 充电中
```

### 协议 C：北鹰方案（V98Pro V3 无线）

VID/PID 为 `0x258A:0x024F` 时使用 `uWe` 控制器：

```js
async getCrc(e) { let n = this.reportId; for (let i = 0; i < e.length; i++) n += e[i]; return n & 255; }
async sendCmd(e, n = 0) {
  let i = new Uint8Array(e.length + 1);
  i.set(e, 0);
  while (i.length < 19) i = new Uint8Array([...i, 0]);
  i[i.length - 1] = await this.getCrc(i);      // 末字节是校验
  return this.sendHidBuffer(i, n, false);      // 走 Output Report
}
async getBattery() { await this.sendCmd(new Uint8Array([74, 1, 0, 0, 0]), 1); }
readBuffer() {  // n[0] == 74 时：level = n[4]，charging = (n[5] >> 4 & 0xF) == 1 }
```

```
发送：Output Report，Report ID = 19，共 19 字节
      payload = 4A 01 00 00 00 00 ... 00，最后一字节 = (19 + 前 18 字节之和) & 0xFF
接收：Input Report 19
      buf[1] == 0x4A → 电量应答
      buf[5] → 电量百分比
      buf[6] >> 4 == 1 → 充电中
```

---

## 四、安装

1. 找到 TrafficMonitor 主程序目录（`TrafficMonitor.exe` 所在目录）。
2. 把 `VGNKeyboardBattery.dll` 复制到该目录下的 **`plugins`** 子目录里
   （若没有 `plugins` 目录，手动新建一个）。
3. 重启 TrafficMonitor。
4. 在任务栏/主窗口上 **右键 → 其他功能 → 插件管理**，确认列表中出现
   **“VGN 键盘电量”**。
5. 再在 **右键 → 显示项目** 里勾选 **“键盘电量”**，即可在任务栏看到数值。

> 键盘必须处于**有线连接**或**已插入 2.4G 接收器**的状态。
> 蓝牙模式下无法通过这种方式读取电量（蓝牙电量走标准 GATT 电池服务，协议不同）。

---

## 五、目录结构

```
VGNKeyboardBattery/
├─ VGNKeyboardBattery.dll      ← 编译产物，直接复制到 TrafficMonitor\plugins\
├─ build.bat                   ← 一键编译脚本（自动识别 MSVC / MinGW）
├─ src/
│  ├─ PluginInterface.h        ← TrafficMonitor 官方插件接口头文件
│  ├─ VgnKeyboardBattery.h/.cpp← 插件本体（ITMPlugin / IPluginItem 实现）
│  └─ VgnHidReader.h/.cpp      ← HID 通信（枚举、封包、解析，纯 Win32 API）
├─ test/
│  ├─ PluginTester.cpp         ← 独立测试器，模拟主程序加载插件
│  └─ HidProbe.cpp             ← HID 协议探针，抓原始报文（调试用）
└─ README.md
```

---

## 六、自己动手编译

插件是标准 C++ 动态库，两种工具链都行（**必须编译成 64 位**，因为 TrafficMonitor 主程序是 64 位）：

### 方式 1：Visual Studio

```bat
cl /nologo /LD /EHsc /O2 /MT /utf-8 /DUNICODE /D_UNICODE /Isrc ^
   src\VgnKeyboardBattery.cpp src\VgnHidReader.cpp ^
   /Fe:VGNKeyboardBattery.dll /link setupapi.lib hid.lib user32.lib
```

### 方式 2：MinGW-w64

```bash
g++ -shared -O2 -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -Isrc \
    src/VgnKeyboardBattery.cpp src/VgnHidReader.cpp -o VGNKeyboardBattery.dll \
    -lsetupapi -lhid -luser32 -lkernel32
```

或者直接双击 `build.bat`。

### 用测试器验证

```bat
cd test
g++ -O2 -DUNICODE -D_UNICODE -I../src PluginTester.cpp -o PluginTester.exe ^
    -lsetupapi -lhid -luser32
PluginTester.exe ..\VGNKeyboardBattery.dll
```

测试器会：
1. 列出本机所有 VID 为 `0x320F` / `0x258A` 的 HID 接口及其 usagePage / 报文长度；
2. 像主程序一样 `LoadLibrary` + `GetProcAddress("TMPluginGetInstance")`；
3. 连续 15 秒每秒调用一次 `DataRequired()` 并打印读到的电量。

如果第 1 步没有列出任何设备，说明键盘没插好或没插 2.4G 接收器；
如果列出了设备但电量一直是 `--`，把第 1 步的输出发我，我来调整协议。

---

## 七、常见问题

**Q：显示“未找到”？**
A：确认键盘已插 USB 数据线，或 2.4G 接收器已插好。蓝牙连接不支持。
   另外确认插件是 64 位版本，且和主程序位数一致。

**Q：一直显示 `--`？**
A：无线键盘空闲一会儿会深度休眠，接收器要“敲门”才能唤醒。
   插件已经做了 1 秒一次的重试，敲几下键盘或按一下键就能唤醒。

**Q：会不会和官方驱动冲突？**
A：不会。插件只是按官方协议发一条查询指令，不写任何配置，也不占用设备独占。

---

## 八、License

MIT
