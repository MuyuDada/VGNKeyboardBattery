// ===========================================================================
//  VgnKeyboardBattery.cpp
//  TrafficMonitor 插件：显示 VGN V98Pro 系列键盘电量
//
//  设计要点：
//    * 所有 HID 通信都在 VgnHidReader 的后台线程里完成，
//      DataRequired() 只读取一份加锁的快照，不会阻塞主程序界面。
//    * 不使用 C++ 标准库 / 动态内存，避免插件 ABI 兼容问题。
// ===========================================================================
#include "VgnKeyboardBattery.h"

// 手写 placement new，避免依赖 C++ 标准库头文件（插件刻意不链接 C++ 运行时）
inline void* operator new(__SIZE_TYPE__, void* p) { return p; }

// ---------------------------------------------------------------------------
//  极简字符串工具（避免依赖 CRT 的格式化函数）
// ---------------------------------------------------------------------------
static void CopyW(wchar_t* dst, const wchar_t* src, int cap)
{
    if (dst == 0 || cap <= 0) return;
    int i = 0;
    if (src != 0)
    {
        for (; i < cap - 1 && src[i] != 0; ++i)
            dst[i] = src[i];
    }
    dst[i] = 0;
}

static int AppendW(wchar_t* buf, int pos, int cap, const wchar_t* s)
{
    if (buf == 0 || s == 0) return pos;
    while (*s != 0 && pos < cap - 1)
        buf[pos++] = *s++;
    buf[pos] = 0;
    return pos;
}

static int AppendIntW(wchar_t* buf, int pos, int cap, int v)
{
    if (buf == 0) return pos;
    if (v < 0) v = 0;
    wchar_t tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = L'0';
    while (v > 0 && n < 11)
    {
        tmp[n++] = (wchar_t)(L'0' + (v % 10));
        v /= 10;
    }
    while (n > 0 && pos < cap - 1)
        buf[pos++] = tmp[--n];
    buf[pos] = 0;
    return pos;
}

// 把 0~100 写成 "85%"
static void FormatPercent(wchar_t* buf, int cap, int v)
{
    if (buf == 0 || cap <= 0) return;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    int pos = AppendIntW(buf, 0, cap, v);
    AppendW(buf, pos, cap, L"%");
}

// ===========================================================================
//  CKeyboardBatteryItem
// ===========================================================================
CKeyboardBatteryItem::CKeyboardBatteryItem()
    : m_graph(0.0f)
{
    CopyW(m_value, L"--", 32);
}

void CKeyboardBatteryItem::Update(bool scanned, bool found, bool online, bool charging, int level)
{
    (void)charging;     // 充电状态放在鼠标提示里展示，显示区域保持简洁
    if (!found)
    {
        // 还没扫描完就先显示 "--"，避免刚加载时闪一下“未找到”
        CopyW(m_value, scanned ? L"未找到" : L"--", 32);
        m_graph = 0.0f;
        return;
    }
    if (level < 0)
    {
        CopyW(m_value, L"--", 32);
        m_graph = 0.0f;
        return;
    }
    FormatPercent(m_value, 32, level);
    m_graph = (float)level / 100.0f;
    (void)online;       // 离线时仍然显示最后一次读到的电量
}

const wchar_t* CKeyboardBatteryItem::GetItemName() const
{
    return L"键盘电量";
}

const wchar_t* CKeyboardBatteryItem::GetItemId() const
{
    // 只包含字母和数字，保证唯一
    return L"VgnKeyboardBattery";
}

const wchar_t* CKeyboardBatteryItem::GetItemLableText() const
{
    return L"键盘";
}

const wchar_t* CKeyboardBatteryItem::GetItemValueText() const
{
    return m_value;
}

const wchar_t* CKeyboardBatteryItem::GetItemValueSampleText() const
{
    return L"100%";
}

int CKeyboardBatteryItem::IsDrawResourceUsageGraph() const
{
    return 1;   // 在任务栏用一条柱状图表示剩余电量
}

float CKeyboardBatteryItem::GetResourceUsageGraphValue() const
{
    return m_graph;
}

// ===========================================================================
//  CVgnKeyboardBatteryPlugin
// ===========================================================================
// 插件实例的静态存储区。
//
// 这里刻意不使用“全局对象 + 静态构造”的写法：那样会依赖 CRT 在 DLL 加载时
// 执行 .init_array 的时机，不同编译工具链 / 宿主程序下并不完全一致。
// 改为在 TMPluginGetInstance() 第一次被调用时原地构造，时机明确、万无一失。
static unsigned char g_pluginStorage[sizeof(CVgnKeyboardBatteryPlugin)] __attribute__((aligned(16)));
static volatile long g_pluginReady = 0;

CVgnKeyboardBatteryPlugin& CVgnKeyboardBatteryPlugin::Instance()
{
    if (g_pluginReady == 0)
    {
        new (g_pluginStorage) CVgnKeyboardBatteryPlugin();
        g_pluginReady = 1;
    }
    return *reinterpret_cast<CVgnKeyboardBatteryPlugin*>(g_pluginStorage);
}

CVgnKeyboardBatteryPlugin::CVgnKeyboardBatteryPlugin()
{
    m_tooltip[0] = 0;
}

IPluginItem* CVgnKeyboardBatteryPlugin::GetItem(int index)
{
    if (index == 0)
        return &m_item;
    return nullptr;
}

void CVgnKeyboardBatteryPlugin::DataRequired()
{
    // 首次调用时启动后台读取线程（幂等）
    m_reader.Start();

    VgnBatteryState st;
    m_reader.GetState(&st);

    m_item.Update(st.scanned, st.deviceFound, st.online, st.charging, st.level);

    // ---- 组装鼠标提示 ----
    int p = 0;
    if (!st.deviceFound)
    {
        if (!st.scanned)
            AppendW(m_tooltip, p, 256, L"正在扫描 VGN 键盘…");
        else
            AppendW(m_tooltip, p, 256,
                    L"未检测到 VGN 键盘。请确认 2.4G 接收器已插入，"
                    L"或键盘已通过数据线连接。");
    }
    else
    {
        p = AppendW(m_tooltip, p, 256, st.deviceName);
        p = AppendW(m_tooltip, p, 256, L"  电量：");
        if (st.level < 0)
        {
            p = AppendW(m_tooltip, p, 256, L"--");
        }
        else
        {
            p = AppendIntW(m_tooltip, p, 256, st.level);
            p = AppendW(m_tooltip, p, 256, L"%");
        }
        if (st.charging)
            p = AppendW(m_tooltip, p, 256, L"  充电中");
        if (!st.online)
            p = AppendW(m_tooltip, p, 256, L"  休眠中");
        if (st.voltage > 0)
        {
            p = AppendW(m_tooltip, p, 256, L"  电压：");
            p = AppendIntW(m_tooltip, p, 256, st.voltage);
            p = AppendW(m_tooltip, p, 256, L" mV");
        }
    }
    m_tooltip[p] = 0;
}

const wchar_t* CVgnKeyboardBatteryPlugin::GetInfo(PluginInfoIndex index)
{
    switch (index)
    {
    case TMI_NAME:        return L"VGN 键盘电量";
    case TMI_DESCRIPTION: return L"在任务栏显示 VGN V98Pro 系列键盘的电池电量（支持有线 / 2.4G）";
    case TMI_AUTHOR:      return L"WorkBuddy AI";
    case TMI_COPYRIGHT:   return L"MIT License";
    case TMI_VERSION:     return L"1.0.0";
    case TMI_URL:         return L"https://hub.vgnlab.com.cn/";
    default:              break;
    }
    return L"";
}

const wchar_t* CVgnKeyboardBatteryPlugin::GetTooltipInfo()
{
    return m_tooltip;
}

int CVgnKeyboardBatteryPlugin::GetCommandCount()
{
    return 1;
}

const wchar_t* CVgnKeyboardBatteryPlugin::GetCommandName(int command_index)
{
    if (command_index == 0)
        return L"立即刷新键盘电量";
    return nullptr;
}

void CVgnKeyboardBatteryPlugin::OnPluginCommand(int command_index, void* hWnd, void* para)
{
    (void)hWnd;
    (void)para;
    if (command_index == 0)
        m_reader.RequestRefresh();
}

void CVgnKeyboardBatteryPlugin::OnInitialize(ITrafficMonitor* pApp)
{
    (void)pApp;
    // 提前启动后台线程，让插件刚加载时就能拿到数据
    m_reader.Start();
}

// ---------------------------------------------------------------------------
//  导出入口
// ---------------------------------------------------------------------------
ITMPlugin* TMPluginGetInstance()
{
    return &CVgnKeyboardBatteryPlugin::Instance();
}
