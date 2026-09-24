// ===========================================================================
//  VgnHidReader.cpp
//  VGN 键盘电量读取器的实现（原生 Win32 HID，无第三方依赖）
//
//  ------------------------------------------------------------------------
//  协议说明（逆向自 VGN HUB 网页驱动 hub.vgnlab.com.cn）
//  ------------------------------------------------------------------------
//
//  【A】维盛(weisheng)方案 —— VGN V98Pro / V98Pro V2
//      VID 0x320F
//        PID 0x5055  有线
//        PID 0x5088  2.4G 接收器
//      Report ID = 4
//      查询：向 OUTPUT report 4 写入 7 字节 [00 00 1A 06 00 00 00]
//            其中 0x1A(26) = GetBatterLevel
//      应答：INPUT report 4，报文（去掉 ReportID 后）：
//            data[2] == 0x1A  时
//            data[7] = 电量百分比(0~100)
//            data[8] = 充电标志(1=充电中)
//      注意：该协议无校验字节。
//
//  【B】北鹰(beiying)方案 —— VGN V98Pro V3 有线
//      VID 0x258A  PID 0x024E
//      Report ID = 9，走 FEATURE report
//      查询：向 FEATURE report 9 写入 520 字节
//            [87 00 00 01 00 02 00 00 ... 00]
//      应答：回读 FEATURE report 9
//            data[8] = 电量百分比
//            data[9] 高 4 位 == 1 表示充电中
//
//  【C】北鹰(beiying)方案 —— VGN V98Pro V3 2.4G 接收器
//      VID 0x258A  PID 0x024F
//      Report ID = 19，走 OUTPUT report
//      查询：向 OUTPUT report 19 写入 19 字节
//            [4A 01 00 00 00 00 ... 校验]
//            校验 = (ReportID + 前 18 字节之和) & 0xFF
//      应答：INPUT report 19
//            data[0] == 0x4A 时
//            data[4] = 电量百分比
//            data[5] 高 4 位 == 1 表示充电中
//
//  ------------------------------------------------------------------------
//  说明：报文在原生 HID 读缓冲里，第 0 个字节是 Report ID，
//        因此 “data[i]” 对应缓冲区里的 buf[i + 1]。
//  ------------------------------------------------------------------------
#include "VgnHidReader.h"

#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <string.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

// HID 设备接口类 GUID（自行定义，避免依赖 initguid.h）
static const GUID kHidInterfaceGuid =
{ 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

// ---------------------------------------------------------------------------
//  协议编号
// ---------------------------------------------------------------------------
enum
{
    PROTO_WEISHENG_OUT   = 0,   // A：OUTPUT report + INPUT report
    PROTO_BEIYING_FEAT   = 1,   // B：FEATURE report 写入 + 回读
    PROTO_BEIYING_OUT    = 2,   // C：OUTPUT report + INPUT report
};

// 查询结果
enum
{
    QR_OK        = 0,   // 拿到有效数据
    QR_NO_REPLY  = 1,   // 没收到应答（设备休眠 / 无线未唤醒）
    QR_DEAD      = 2,   // 通道失效，需要重新枚举
};

// ---------------------------------------------------------------------------
//  支持的设备表
// ---------------------------------------------------------------------------
struct DeviceDef
{
    unsigned short vid;
    unsigned short pid;
    int            proto;
    int            reportId;
    const wchar_t* name;
};

static const DeviceDef kDevices[] =
{
    { 0x320F, 0x5055, PROTO_WEISHENG_OUT,  4, L"VGN V98Pro"            },
    { 0x320F, 0x5088, PROTO_WEISHENG_OUT,  4, L"VGN V98Pro (2.4G)"     },
    { 0x258A, 0x024E, PROTO_BEIYING_FEAT,  9, L"VGN V98Pro V3"         },
    { 0x258A, 0x024F, PROTO_BEIYING_OUT,  19, L"VGN V98Pro V3 (2.4G)"  },
};

static const int kDeviceCount = (int)(sizeof(kDevices) / sizeof(kDevices[0]));

// ---------------------------------------------------------------------------
//  小工具
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

static bool IsEmptyW(const wchar_t* s)
{
    return s == 0 || s[0] == 0;
}

static void SleepMs(DWORD ms)
{
    Sleep(ms);
}

// ---------------------------------------------------------------------------
//  构造 / 析构
// ---------------------------------------------------------------------------
VgnHidReader::VgnHidReader()
    : m_thread(0)
    , m_stopEvent(0)
    , m_wakeEvent(0)
    , m_lockReady(false)
    , m_started(false)
    , m_dev(INVALID_HANDLE_VALUE)
    , m_proto(-1)
    , m_reportId(0)
    , m_outLen(0)
    , m_inLen(0)
    , m_featLen(0)
{
    m_path[0] = 0;
    InitializeCriticalSection(&m_lock);
    m_lockReady = true;

    memset(&m_state, 0, sizeof(m_state));
    m_state.level = -1;

    m_stopEvent = CreateEventW(0, TRUE, FALSE, 0);   // 手动重置
    m_wakeEvent = CreateEventW(0, TRUE, FALSE, 0);   // 手动重置
}

// ---------------------------------------------------------------------------
//  线程控制
// ---------------------------------------------------------------------------
void VgnHidReader::Start()
{
    if (m_started) return;
    if (m_stopEvent == 0) m_stopEvent = CreateEventW(0, TRUE, FALSE, 0);
    if (m_wakeEvent == 0) m_wakeEvent = CreateEventW(0, TRUE, FALSE, 0);
    if (m_stopEvent == 0) return;

    ResetEvent(m_stopEvent);
    m_started = true;

    DWORD tid = 0;
    m_thread = CreateThread(0, 0, &VgnHidReader::ThreadProc, this, 0, &tid);
    if (m_thread == 0)
        m_started = false;
}

void VgnHidReader::Stop()
{
    if (!m_started)
    {
        CloseDevice();
        return;
    }
    if (m_stopEvent) SetEvent(m_stopEvent);
    if (m_wakeEvent) SetEvent(m_wakeEvent);

    if (m_thread)
    {
        WaitForSingleObject(m_thread, 3000);
        CloseHandle(m_thread);
        m_thread = 0;
    }
    m_started = false;
    CloseDevice();
}

void VgnHidReader::RequestRefresh()
{
    if (m_wakeEvent) SetEvent(m_wakeEvent);
}

DWORD WINAPI VgnHidReader::ThreadProc(LPVOID param)
{
    VgnHidReader* self = (VgnHidReader*)param;
    self->WorkerLoop();
    return 0;
}

void VgnHidReader::WorkerLoop()
{
    // 启动后立刻查询一次
    PollOnce();

    for (;;)
    {
        // 在线时 5 秒轮询一次；离线（休眠/未连接）时 1 秒一次，用于唤醒设备
        bool online = false;
        if (m_lockReady)
        {
            EnterCriticalSection(&m_lock);
            online = m_state.online;
            LeaveCriticalSection(&m_lock);
        }
        DWORD interval = online ? 5000 : 1000;

        DWORD w = WaitForSingleObject(m_wakeEvent, interval);
        if (WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0)
            break;
        if (w == WAIT_OBJECT_0)
            ResetEvent(m_wakeEvent);

        PollOnce();
    }
}

// ---------------------------------------------------------------------------
//  状态读写
// ---------------------------------------------------------------------------
void VgnHidReader::SetState(bool found, bool online, bool charging, int level,
                            int voltage, const wchar_t* name)
{
    if (!m_lockReady) return;
    EnterCriticalSection(&m_lock);
    m_state.scanned     = true;
    m_state.deviceFound = found;
    m_state.online      = online;
    m_state.charging    = charging;
    m_state.level       = level;
    m_state.voltage     = voltage;
    if (name != 0)
        CopyW(m_state.deviceName, name, 64);
    LeaveCriticalSection(&m_lock);
}

void VgnHidReader::GetState(VgnBatteryState* out)
{
    if (out == 0) return;
    if (!m_lockReady)
    {
        memset(out, 0, sizeof(*out));
        out->level = -1;
        return;
    }
    EnterCriticalSection(&m_lock);
    *out = m_state;
    LeaveCriticalSection(&m_lock);
}

void VgnHidReader::ClearDeviceInfo()
{
    m_path[0] = 0;
    m_proto = -1;
    m_reportId = 0;
    m_outLen = m_inLen = m_featLen = 0;
}

// ---------------------------------------------------------------------------
//  设备开关
// ---------------------------------------------------------------------------
void VgnHidReader::CloseDevice()
{
    if (m_dev != 0 && m_dev != INVALID_HANDLE_VALUE)
        CloseHandle(m_dev);
    m_dev = INVALID_HANDLE_VALUE;
}

// ---------------------------------------------------------------------------
//  枚举 HID 接口并锁定可用通道
// ---------------------------------------------------------------------------
struct Candidate
{
    wchar_t        path[512];
    int            proto;
    int            reportId;
    int            outLen;
    int            inLen;
    int            featLen;
    unsigned short usagePage;
    const wchar_t* name;
    bool           vendor;      // usagePage 是否落在厂商自定义页 0xFF00~0xFFFF
};

// 打开指定路径的 HID 接口
static HANDLE OpenHidPath(const wchar_t* path)
{
    return CreateFileW(path,
                       GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       0,
                       OPEN_EXISTING,
                       FILE_FLAG_OVERLAPPED,
                       0);
}

// 读取一次 Input report，带超时
static int ReadInputReport(HANDLE dev, unsigned char* buf, DWORD len, DWORD timeoutMs)
{
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(0, TRUE, FALSE, 0);
    if (ov.hEvent == 0) return QR_DEAD;

    DWORD got = 0;
    int   rc  = QR_NO_REPLY;
    BOOL  ok  = ReadFile(dev, buf, len, &got, &ov);

    if (ok)
    {
        rc = (got > 0) ? QR_OK : QR_NO_REPLY;
    }
    else
    {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING)
        {
            DWORD w = WaitForSingleObject(ov.hEvent, timeoutMs);
            if (w == WAIT_OBJECT_0)
            {
                if (GetOverlappedResult(dev, &ov, &got, FALSE))
                    rc = (got > 0) ? QR_OK : QR_NO_REPLY;
                else
                    rc = QR_DEAD;
            }
            else
            {
                CancelIo(dev);
                rc = QR_NO_REPLY;
            }
        }
        else if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_INVALID_HANDLE)
        {
            rc = QR_DEAD;
        }
        else
        {
            rc = QR_DEAD;
        }
    }

    CloseHandle(ov.hEvent);
    return rc;
}

// 发送一个 Output report（带超时）
//
// 注意：这里必须用 WriteFile，不能用 HidD_SetOutputReport！
// 实测该键盘的厂商通道只在“中断 OUT 端点”上接收指令：
//   HidD_SetOutputReport 走的是 IOCTL_HID_SET_OUTPUT_REPORT（控制传输），
//   调用会返回成功，但设备完全不理；
//   而 WriteFile 走中断 OUT 传输，设备才会正常回包。
static int WriteOutputReport(HANDLE dev, const unsigned char* buf, DWORD len, DWORD timeoutMs)
{
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(0, TRUE, FALSE, 0);
    if (ov.hEvent == 0) return QR_DEAD;

    DWORD written = 0;
    int   rc = QR_DEAD;
    BOOL  ok = WriteFile(dev, buf, len, &written, &ov);

    if (ok)
    {
        rc = QR_OK;
    }
    else
    {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_OBJECT_0)
            {
                DWORD dw = 0;
                rc = GetOverlappedResult(dev, &ov, &dw, FALSE) ? QR_OK : QR_DEAD;
            }
            else
            {
                CancelIo(dev);
                rc = QR_DEAD;
            }
        }
        else
        {
            rc = QR_DEAD;
        }
    }

    CloseHandle(ov.hEvent);
    return rc;
}

bool VgnHidReader::EnumerateAndOpen()
{
    Candidate cands[16];
    int candCount = 0;

    HDEVINFO devInfo = SetupDiGetClassDevsW(&kHidInterfaceGuid, 0, 0,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE)
        return false;

    SP_DEVICE_INTERFACE_DATA did;
    memset(&did, 0, sizeof(did));
    did.cbSize = sizeof(did);

    for (DWORD i = 0; candCount < 16 &&
                     SetupDiEnumDeviceInterfaces(devInfo, 0, &kHidInterfaceGuid, i, &did);
         ++i)
    {
        // 取接口路径
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &did, 0, 0, &need, 0);
        if (need == 0 || need > 2048) continue;

        unsigned char detailBuf[2048];
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* detail =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)detailBuf;
        memset(detailBuf, 0, sizeof(detailBuf));
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &did, detail, need, 0, 0))
            continue;

        const wchar_t* path = detail->DevicePath;

        HANDLE dev = OpenHidPath(path);
        if (dev == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attr;
        memset(&attr, 0, sizeof(attr));
        attr.Size = sizeof(attr);
        if (!HidD_GetAttributes(dev, &attr))
        {
            CloseHandle(dev);
            continue;
        }

        // 与设备表比对
        const DeviceDef* def = 0;
        for (int k = 0; k < kDeviceCount; ++k)
        {
            if (kDevices[k].vid == attr.VendorID && kDevices[k].pid == attr.ProductID)
            {
                def = &kDevices[k];
                break;
            }
        }
        if (def == 0)
        {
            CloseHandle(dev);
            continue;
        }

        // 取 HID 能力
        PHIDP_PREPARSED_DATA ppd = 0;
        HIDP_CAPS caps;
        memset(&caps, 0, sizeof(caps));
        if (!HidD_GetPreparsedData(dev, &ppd) ||
            HidP_GetCaps(ppd, &caps) != HIDP_STATUS_SUCCESS)
        {
            if (ppd) HidD_FreePreparsedData(ppd);
            CloseHandle(dev);
            continue;
        }
        HidD_FreePreparsedData(ppd);

        Candidate& c = cands[candCount];
        memset(&c, 0, sizeof(c));
        CopyW(c.path, path, 512);
        c.proto     = def->proto;
        c.reportId  = def->reportId;
        c.outLen    = (int)caps.OutputReportByteLength;
        c.inLen     = (int)caps.InputReportByteLength;
        c.featLen   = (int)caps.FeatureReportByteLength;
        c.usagePage = caps.UsagePage;
        c.name      = def->name;
        c.vendor    = (caps.UsagePage >= 0xFF00);
        ++candCount;

        CloseHandle(dev);
    }

    SetupDiDestroyDeviceInfoList(devInfo);

    if (candCount == 0)
    {
        SetState(false, false, false, -1, 0, L"");
        return false;
    }

    // 把“厂商自定义页”的接口排在前面，优先尝试
    for (int a = 0; a < candCount - 1; ++a)
    {
        for (int b = 0; b < candCount - 1 - a; ++b)
        {
            if (cands[b].vendor == false && cands[b + 1].vendor == true)
            {
                Candidate t = cands[b];
                cands[b] = cands[b + 1];
                cands[b + 1] = t;
            }
        }
    }

    // 逐个尝试建立通道
    for (int k = 0; k < candCount; ++k)
    {
        Candidate& c = cands[k];

        HANDLE dev = OpenHidPath(c.path);
        if (dev == INVALID_HANDLE_VALUE) continue;

        m_dev      = dev;
        m_proto    = c.proto;
        m_reportId = c.reportId;
        m_outLen   = c.outLen;
        m_inLen    = c.inLen;
        m_featLen  = c.featLen;
        CopyW(m_path, c.path, 512);

        int level = -1, charging = 0, voltage = 0;
        int r = QueryBattery(&level, &charging, &voltage);

        if (r == QR_OK)
        {
            SetState(true, true, charging != 0, level, voltage, c.name);
            return true;
        }

        // 通道打开成功但没有数据 —— 记下来，等下一轮再试（设备可能正在休眠）
        if (r == QR_NO_REPLY)
        {
            CloseDevice();
            SetState(true, false, false, -1, 0, c.name);
            return true;
        }

        // 通道失效，换下一个
        CloseDevice();
        ClearDeviceInfo();
    }

    ClearDeviceInfo();
    SetState(false, false, false, -1, 0, L"");
    return false;
}

// ---------------------------------------------------------------------------
//  一次查询
// ---------------------------------------------------------------------------
int VgnHidReader::QueryBattery(int* level, int* charging, int* voltage)
{
    if (level) *level = -1;
    if (charging) *charging = 0;
    if (voltage) *voltage = 0;
    if (m_dev == 0 || m_dev == INVALID_HANDLE_VALUE) return QR_DEAD;

    static unsigned char wbuf[1024];
    static unsigned char rbuf[1024];

    int    outTotal = m_outLen > 0 ? m_outLen : 64;
    if (outTotal > (int)sizeof(wbuf)) outTotal = (int)sizeof(wbuf);
    int    inTotal  = m_inLen > 0 ? m_inLen : 64;
    if (inTotal > (int)sizeof(rbuf)) inTotal = (int)sizeof(rbuf);
    int    featTotal = m_featLen > 0 ? m_featLen : 64;
    if (featTotal > (int)sizeof(wbuf)) featTotal = (int)sizeof(wbuf);

    // ---------------------------------------------------------------
    // A：维盛方案 —— OUTPUT report 4
    // ---------------------------------------------------------------
    if (m_proto == PROTO_WEISHENG_OUT)
    {
        static const unsigned char query[7] = { 0x00, 0x00, 0x1A, 0x06, 0x00, 0x00, 0x00 };

        memset(wbuf, 0, sizeof(wbuf));
        wbuf[0] = (unsigned char)m_reportId;
        memcpy(wbuf + 1, query, sizeof(query));

        if (WriteOutputReport(m_dev, wbuf, (DWORD)outTotal, 500) != QR_OK)
            return QR_DEAD;

        // 读应答（设备偶尔会先回一个无关报文，多读几次）
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            memset(rbuf, 0, sizeof(rbuf));
            int rc = ReadInputReport(m_dev, rbuf, (DWORD)inTotal, 300);
            if (rc == QR_DEAD) return QR_DEAD;
            if (rc == QR_OK && rbuf[0] == (unsigned char)m_reportId && rbuf[3] == 0x1A)
            {
                // 报文（去掉 Report ID）：
                //   data[7] = 电量百分比   -> buf[8]
                //   data[8] = 充电标志     -> buf[9]
                if (level)    *level = (int)rbuf[8];
                if (charging) *charging = (rbuf[9] != 0) ? 1 : 0;
                return QR_OK;
            }
        }
        return QR_NO_REPLY;
    }

    // ---------------------------------------------------------------
    // B：北鹰方案（V3 有线）—— FEATURE report 9 写入 + 回读
    // ---------------------------------------------------------------
    if (m_proto == PROTO_BEIYING_FEAT)
    {
        static const unsigned char query[7] = { 0x87, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00 };

        memset(wbuf, 0, sizeof(wbuf));
        wbuf[0] = (unsigned char)m_reportId;
        memcpy(wbuf + 1, query, sizeof(query));

        if (!HidD_SetFeature(m_dev, wbuf, (ULONG)featTotal))
        {
            DWORD err = GetLastError();
            if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_INVALID_HANDLE ||
                err == ERROR_ACCESS_DENIED)
                return QR_DEAD;
            if (!HidD_SetFeature(m_dev, wbuf, (ULONG)(1 + sizeof(query))))
                return QR_DEAD;
        }

        SleepMs(25);

        memset(rbuf, 0, sizeof(rbuf));
        if (!HidD_GetFeature(m_dev, rbuf, (ULONG)featTotal))
            return QR_NO_REPLY;

        // rbuf[0] = ReportID，data[i] == rbuf[i + 1]
        if (rbuf[0] == (unsigned char)m_reportId && rbuf[1] == 0x87)
        {
            if (level)    *level = (int)rbuf[9];
            if (charging) *charging = ((rbuf[10] >> 4) & 0x0F) == 1 ? 1 : 0;
            return QR_OK;
        }
        return QR_NO_REPLY;
    }

    // ---------------------------------------------------------------
    // C：北鹰方案（V3 无线）—— OUTPUT report 19 + 校验
    // ---------------------------------------------------------------
    if (m_proto == PROTO_BEIYING_OUT)
    {
        unsigned char payload[19];
        memset(payload, 0, sizeof(payload));
        payload[0] = 0x4A;
        payload[1] = 0x01;

        // 校验 = (ReportID + 前 18 字节之和) & 0xFF
        unsigned int sum = (unsigned int)m_reportId;
        for (int i = 0; i < 18; ++i)
            sum += payload[i];
        payload[18] = (unsigned char)(sum & 0xFF);

        memset(wbuf, 0, sizeof(wbuf));
        wbuf[0] = (unsigned char)m_reportId;
        memcpy(wbuf + 1, payload, sizeof(payload));

        if (WriteOutputReport(m_dev, wbuf, (DWORD)outTotal, 500) != QR_OK)
            return QR_DEAD;

        for (int attempt = 0; attempt < 4; ++attempt)
        {
            memset(rbuf, 0, sizeof(rbuf));
            int rc = ReadInputReport(m_dev, rbuf, (DWORD)inTotal, 300);
            if (rc == QR_DEAD) return QR_DEAD;
            if (rc == QR_OK && rbuf[0] == (unsigned char)m_reportId && rbuf[1] == 0x4A)
            {
                if (level)    *level = (int)rbuf[5];
                if (charging) *charging = ((rbuf[6] >> 4) & 0x0F) == 1 ? 1 : 0;
                return QR_OK;
            }
        }
        return QR_NO_REPLY;
    }

    return QR_DEAD;
}

// ---------------------------------------------------------------------------
//  单轮轮询
// ---------------------------------------------------------------------------
void VgnHidReader::PollOnce()
{
    // 还没锁定通道
    if (m_dev == 0 || m_dev == INVALID_HANDLE_VALUE)
    {
        if (IsEmptyW(m_path))
        {
            // 完全没线索 —— 重新枚举
            EnumerateAndOpen();
            return;
        }

        // 之前锁过通道（设备休眠），直接重开
        HANDLE dev = OpenHidPath(m_path);
        if (dev == INVALID_HANDLE_VALUE)
        {
            ClearDeviceInfo();
            EnumerateAndOpen();
            return;
        }
        m_dev = dev;
    }

    int level = -1, charging = 0, voltage = 0;
    int r = QueryBattery(&level, &charging, &voltage);

    if (r == QR_OK)
    {
        // 保留上一次的设备名
        VgnBatteryState cur;
        GetState(&cur);
        SetState(true, true, charging != 0, level, voltage, cur.deviceName);
    }
    else if (r == QR_NO_REPLY)
    {
        // 设备休眠 —— 保留通道，仅标记为离线
        VgnBatteryState cur;
        GetState(&cur);
        SetState(cur.deviceFound, false, false, cur.level, cur.voltage, cur.deviceName);
    }
    else
    {
        // 通道失效
        CloseDevice();
        ClearDeviceInfo();
        VgnBatteryState cur;
        GetState(&cur);
        SetState(cur.deviceFound, false, false, -1, 0, cur.deviceName);
    }
}
