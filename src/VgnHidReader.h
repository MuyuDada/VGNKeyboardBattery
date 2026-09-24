// ===========================================================================
//  VgnHidReader.h
//  VGN 键盘电量读取器（后台线程 + 原生 Win32 HID API，无第三方依赖）
//
//  逆向自 VGN 官方网页驱动（VGN HUB）的 HID 通信协议。
//  支持：
//    - VGN V98Pro / V98Pro V2       （维盛方案，VID 0x320F）
//    - VGN V98Pro V3                （北鹰方案，VID 0x258A）
//  协议细节见 README.md。
// ===========================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// ---------------------------------------------------------------------------
//  电量状态快照
// ---------------------------------------------------------------------------
struct VgnBatteryState
{
    bool scanned;            // 是否已经完成过至少一次设备扫描
    bool deviceFound;        // 是否枚举到受支持的键盘/接收器
    bool online;             // 键盘是否在线（无线休眠时可能为 false）
    bool charging;           // 是否正在充电
    int  level;              // 电量百分比 0~100；-1 表示未知
    int  voltage;            // 电压(mV)；0 表示未知
    wchar_t deviceName[64];  // 匹配到的设备名
};

// ---------------------------------------------------------------------------
//  后台读取器
//     - Start() 启动工作线程，线程会周期性地向键盘发送电量查询指令
//     - GetState() 是线程安全的，可在任意线程（含 UI 线程）调用
// ---------------------------------------------------------------------------
class VgnHidReader
{
public:
    // 注意：刻意不提供析构函数。
    // 插件对象是进程级全局对象，生命周期与主程序一致；
    // 进程退出时操作系统会回收线程与句柄，因此无需（也不应）注册退出清理回调。
    VgnHidReader();

    void Start();                       // 启动后台线程（可重复调用，幂等）
    void Stop();                        // 停止后台线程并释放设备
    void RequestRefresh();              // 请求立即刷新一次
    void GetState(VgnBatteryState* out);// 读取当前状态（线程安全）

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    void WorkerLoop();
    void PollOnce();

    // 枚举匹配的 HID 接口并尝试建立可用通道；成功返回 true
    bool EnumerateAndOpen();

    // 查询一次电量。返回 0=成功，1=无响应(设备休眠)，2=通道失效需重连
    int  QueryBattery(int* level, int* charging, int* voltage);

    void CloseDevice();
    void SetState(bool found, bool online, bool charging, int level, int voltage,
                  const wchar_t* name);
    void ClearDeviceInfo();

    HANDLE m_thread;
    HANDLE m_stopEvent;
    HANDLE m_wakeEvent;
    CRITICAL_SECTION m_lock;
    bool   m_lockReady;
    bool   m_started;

    // 当前锁定的设备通道
    HANDLE m_dev;
    int    m_proto;         // 协议编号，见 VgnHidReader.cpp 中的 PROTO_*
    int    m_reportId;      // 该协议使用的 HID Report ID
    int    m_outLen;        // OutputReportByteLength
    int    m_inLen;         // InputReportByteLength
    int    m_featLen;       // FeatureReportByteLength
    wchar_t m_path[512];    // 已打开的接口路径，用于重连

    VgnBatteryState m_state;
};
