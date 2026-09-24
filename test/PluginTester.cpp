// ===========================================================================
//  PluginTester.cpp
//  一个极简的插件测试器：完全模仿 TrafficMonitor 主程序加载插件的方式，
//  用于在没有主程序的情况下验证插件是否能被正确加载、导出函数是否正常、
//  以及能否读到键盘电量。
//
//  用法：PluginTester.exe [插件dll路径]
// ===========================================================================
#include <windows.h>
#include <stdio.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <string.h>

#include "PluginInterface.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

// ---------------------------------------------------------------------------
static void P(const wchar_t* s)
{
    if (s == 0) { printf("(null)\n"); return; }
    char buf[2048];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf) - 1, 0, 0);
    if (n <= 0) { printf("(convert fail)\n"); return; }
    buf[n] = 0;
    fputs(buf, stdout);
}

static void PLine(const wchar_t* s) { P(s); printf("\n"); }

// ---------------------------------------------------------------------------
//  诊断：列出本机所有 VID 匹配的 HID 接口
// ---------------------------------------------------------------------------
static void DumpHidInterfaces()
{
    printf("---- HID 接口诊断 ----\n");
    static const GUID hidGuid =
    { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

    HDEVINFO di = SetupDiGetClassDevsW(&hidGuid, 0, 0, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) { printf("SetupDiGetClassDevs 失败\n"); return; }

    SP_DEVICE_INTERFACE_DATA did;
    memset(&did, 0, sizeof(did));
    did.cbSize = sizeof(did);

    int found = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, 0, &hidGuid, i, &did); ++i)
    {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(di, &did, 0, 0, &need, 0);
        if (need == 0 || need > 4096) continue;

        unsigned char* buf = (unsigned char*)malloc(need);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* d = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf;
        memset(buf, 0, need);
        d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(di, &did, d, need, 0, 0)) { free(buf); continue; }

        HANDLE h = CreateFileW(d->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, 0);
        if (h != INVALID_HANDLE_VALUE)
        {
            HIDD_ATTRIBUTES at;
            memset(&at, 0, sizeof(at));
            at.Size = sizeof(at);
            if (HidD_GetAttributes(h, &at))
            {
                if (at.VendorID == 0x320F || at.VendorID == 0x258A)
                {
                    PHIDP_PREPARSED_DATA ppd = 0;
                    HIDP_CAPS caps;
                    memset(&caps, 0, sizeof(caps));
                    int okCaps = 0;
                    if (HidD_GetPreparsedData(h, &ppd) &&
                        HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS)
                        okCaps = 1;
                    if (ppd) HidD_FreePreparsedData(ppd);

                    printf("  VID=%04X PID=%04X  usagePage=%04X usage=%04X  "
                           "In=%u Out=%u Feat=%u\n",
                           at.VendorID, at.ProductID,
                           okCaps ? caps.UsagePage : 0, okCaps ? caps.Usage : 0,
                           okCaps ? caps.InputReportByteLength : 0,
                           okCaps ? caps.OutputReportByteLength : 0,
                           okCaps ? caps.FeatureReportByteLength : 0);
                    ++found;
                }
            }
            CloseHandle(h);
        }
        free(buf);
    }
    SetupDiDestroyDeviceInfoList(di);

    if (found == 0)
        printf("  （没有发现 VID 为 0x320F / 0x258A 的 HID 设备）\n");
    printf("\n");
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    const wchar_t* dllPath = L"VGNKeyboardBattery.dll";
    wchar_t dllBuf[MAX_PATH] = L"VGNKeyboardBattery.dll";
    if (argc > 1)
    {
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, dllBuf, MAX_PATH);
        dllPath = dllBuf;
    }

    printf("==================================================\n");
    printf(" TrafficMonitor 插件测试器 - VGN 键盘电量\n");
    printf("==================================================\n\n");

    DumpHidInterfaces();

    HMODULE hMod = LoadLibraryW(dllPath);
    if (hMod == 0)
    {
        printf("[错误] 无法加载插件 DLL：%s (GetLastError=%lu)\n",
               argc > 1 ? argv[1] : "VGNKeyboardBattery.dll", GetLastError());
        return 1;
    }
    printf("[OK] 插件 DLL 已加载\n");

    typedef ITMPlugin* (*GetInstanceFn)();
    GetInstanceFn getInstance =
        (GetInstanceFn)(void*)GetProcAddress(hMod, "TMPluginGetInstance");
    if (getInstance == 0)
    {
        printf("[错误] 未找到导出函数 TMPluginGetInstance\n");
        FreeLibrary(hMod);
        return 1;
    }
    printf("[OK] 已找到导出函数 TMPluginGetInstance\n");

    ITMPlugin* plugin = getInstance();
    if (plugin == 0) { printf("[错误] TMPluginGetInstance 返回空指针\n"); return 1; }
    printf("[OK] 插件实例 = %p，接口版本 = %d\n\n", (void*)plugin, plugin->GetAPIVersion());

    printf("---- 插件信息 ----\n");
    printf("  名称   : "); PLine(plugin->GetInfo(ITMPlugin::TMI_NAME));
    printf("  描述   : "); PLine(plugin->GetInfo(ITMPlugin::TMI_DESCRIPTION));
    printf("  作者   : "); PLine(plugin->GetInfo(ITMPlugin::TMI_AUTHOR));
    printf("  版本   : "); PLine(plugin->GetInfo(ITMPlugin::TMI_VERSION));
    printf("  主页   : "); PLine(plugin->GetInfo(ITMPlugin::TMI_URL));
    printf("\n");

    printf("---- 显示项目 ----\n");
    for (int i = 0; i < 4; ++i)
    {
        IPluginItem* item = plugin->GetItem(i);
        if (item == 0)
        {
            if (i == 0) printf("  [错误] GetItem(0) 返回空指针\n");
            break;
        }
        printf("  [%d] 名称=", i); P(item->GetItemName());
        printf("  ID="); P(item->GetItemId());
        printf("  标签="); P(item->GetItemLableText());
        printf("  示例="); P(item->GetItemValueSampleText());
        printf("  自绘=%d  电量条=%d\n",
               item->IsCustomDraw() ? 1 : 0, item->IsDrawResourceUsageGraph());
    }
    printf("\n");

    printf("---- 开始轮询（每 1 秒调用一次 DataRequired，共 15 次）----\n");
    for (int t = 0; t < 15; ++t)
    {
        plugin->DataRequired();
        IPluginItem* item = plugin->GetItem(0);
        printf("  [%2ds] 数值=", t + 1);
        P(item->GetItemValueText());
        printf("   电量条=%.2f   提示=", (double)item->GetResourceUsageGraphValue());
        PLine(plugin->GetTooltipInfo());
        fflush(stdout);
        Sleep(1000);
    }

    printf("\n测试结束。\n");
    FreeLibrary(hMod);
    return 0;
}
