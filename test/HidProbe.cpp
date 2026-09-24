// ===========================================================================
//  HidProbe.cpp
//  HID 协议探针：直接对 VGN 键盘的厂商通道做原始读写，打印十六进制报文。
//  用于在没有官方驱动的情况下确认正确的 Report ID / 指令 / 回包格式。
// ===========================================================================
#include <windows.h>
#include <stdio.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <string.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

static const GUID kHidGuid =
{ 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

static void HexDump(const char* tag, const unsigned char* p, int n)
{
    printf("%s (%d): ", tag, n);
    for (int i = 0; i < n; ++i) printf("%02X ", p[i]);
    printf("\n");
}

// 枚举某个 prepdata 下所有 Report ID
static void DumpReportIds(PHIDP_PREPARSED_DATA ppd, HIDP_CAPS* caps, const char* kind,
                          HIDP_REPORT_TYPE type, USHORT count)
{
    if (count == 0) { printf("    %s report: 无（仅 Report ID 0）\n", kind); return; }
    HIDP_VALUE_CAPS vc[256];
    USHORT n = count > 256 ? 256 : count;
    memset(vc, 0, sizeof(vc));
    if (HidP_GetValueCaps(type, vc, &n, ppd) == HIDP_STATUS_SUCCESS)
    {
        printf("    %s report IDs:", kind);
        for (USHORT i = 0; i < n; ++i) printf(" %u", vc[i].ReportID);
        printf("\n");
    }
    USHORT nb = (type == HidP_Input) ? caps->NumberInputButtonCaps
                                     : (type == HidP_Output) ? caps->NumberOutputButtonCaps
                                                             : caps->NumberFeatureButtonCaps;
    if (nb > 0)
    {
        HIDP_BUTTON_CAPS bc[256];
        USHORT m = nb > 256 ? 256 : nb;
        memset(bc, 0, sizeof(bc));
        if (HidP_GetButtonCaps(type, bc, &m, ppd) == HIDP_STATUS_SUCCESS)
        {
            printf("    %s button report IDs:", kind);
            for (USHORT i = 0; i < m; ++i) printf(" %u", bc[i].ReportID);
            printf("\n");
        }
    }
}

static int ReadWithTimeout(HANDLE h, unsigned char* buf, DWORD len, DWORD ms)
{
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(0, TRUE, FALSE, 0);
    DWORD got = 0;
    int rc = 0;
    BOOL ok = ReadFile(h, buf, len, &got, &ov);
    if (ok) { rc = (int)got; }
    else if (GetLastError() == ERROR_IO_PENDING)
    {
        if (WaitForSingleObject(ov.hEvent, ms) == WAIT_OBJECT_0)
        {
            if (GetOverlappedResult(h, &ov, &got, FALSE)) rc = (int)got;
            else rc = -2;
        }
        else { CancelIo(h); rc = -1; }
    }
    else rc = -3;
    CloseHandle(ov.hEvent);
    return rc;
}

int main(void)
{
    SetConsoleOutputCP(CP_UTF8);
    printf("=== VGN HID 协议探针 ===\n\n");

    HDEVINFO di = SetupDiGetClassDevsW(&kHidGuid, 0, 0, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) { printf("枚举失败\n"); return 1; }

    SP_DEVICE_INTERFACE_DATA did;
    memset(&did, 0, sizeof(did));
    did.cbSize = sizeof(did);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, 0, &kHidGuid, i, &did); ++i)
    {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(di, &did, 0, 0, &need, 0);
        if (need == 0 || need > 4096) continue;

        unsigned char* dbuf = (unsigned char*)malloc(need);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* d = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)dbuf;
        memset(dbuf, 0, need);
        d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(di, &did, d, need, 0, 0)) { free(dbuf); continue; }

        HANDLE h = CreateFileW(d->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, 0);
        if (h == INVALID_HANDLE_VALUE) { free(dbuf); continue; }

        HIDD_ATTRIBUTES at;
        memset(&at, 0, sizeof(at));
        at.Size = sizeof(at);
        if (!HidD_GetAttributes(h, &at) || at.VendorID != 0x320F)
        {
            CloseHandle(h); free(dbuf); continue;
        }

        PHIDP_PREPARSED_DATA ppd = 0;
        HIDP_CAPS caps;
        memset(&caps, 0, sizeof(caps));
        if (!HidD_GetPreparsedData(h, &ppd) || HidP_GetCaps(ppd, &caps) != HIDP_STATUS_SUCCESS)
        {
            CloseHandle(h); free(dbuf); continue;
        }

        printf("--------------------------------------------------------------\n");
        printf("接口: usagePage=%04X usage=%04X  In=%u Out=%u Feat=%u\n",
               caps.UsagePage, caps.Usage,
               caps.InputReportByteLength, caps.OutputReportByteLength,
               caps.FeatureReportByteLength);
        DumpReportIds(ppd, &caps, "Input", HidP_Input, caps.NumberInputValueCaps);
        DumpReportIds(ppd, &caps, "Output", HidP_Output, caps.NumberOutputValueCaps);
        DumpReportIds(ppd, &caps, "Feature", HidP_Feature, caps.NumberFeatureValueCaps);

        // 只对厂商自定义通道（usagePage >= 0xFF00）做报文实验
        if (caps.UsagePage >= 0xFF00 && caps.OutputReportByteLength > 0)
        {
            printf("\n  >>> 在该通道上尝试发送电量查询指令 <<<\n");

            int outLen = caps.OutputReportByteLength;
            int inLen = caps.InputReportByteLength;
            if (outLen > 512) outLen = 512;
            if (inLen > 512) inLen = 512;

            unsigned char wbuf[512];
            unsigned char rbuf[512];

            static const int tryIds[] = { 4, 0, 1, 2, 3, 5, 6, 7, 8 };
            static const char* tryNames[] = { "0x1A电量", "0x1A电量", "0x1A电量", "0x1A电量",
                                              "0x1A电量", "0x1A电量", "0x1A电量", "0x1A电量",
                                              "0x1A电量" };

            for (int k = 0; k < (int)(sizeof(tryIds) / sizeof(tryIds[0])); ++k)
            {
                int rid = tryIds[k];
                memset(wbuf, 0, sizeof(wbuf));
                wbuf[0] = (unsigned char)rid;
                wbuf[1] = 0x00;
                wbuf[2] = 0x00;
                wbuf[3] = 0x1A;   // GetBatterLevel
                wbuf[4] = 0x06;
                wbuf[5] = 0x00;
                wbuf[6] = 0x00;
                wbuf[7] = 0x00;

                SetLastError(0);
                BOOL w1 = HidD_SetOutputReport(h, wbuf, (ULONG)outLen);
                DWORD e1 = GetLastError();
                printf("  [%s] ReportID=%d  SetOutputReport(len=%d) = %s (err=%lu)\n",
                       tryNames[k], rid, outLen, w1 ? "OK" : "FAIL", e1);
                if (!w1) continue;

                // 尝试读取回包
                for (int attempt = 0; attempt < 2; ++attempt)
                {
                    memset(rbuf, 0, sizeof(rbuf));
                    int rc = ReadWithTimeout(h, rbuf, (DWORD)inLen, 400);
                    if (rc > 0)
                    {
                        HexDump("    收到回包", rbuf, rc);
                        if (rbuf[3] == 0x1A)
                            printf("    >>> 命中！电量 = %d%%, 充电 = %d\n", rbuf[8], rbuf[9]);
                        break;
                    }
                    else
                    {
                        printf("    读超时/失败 rc=%d err=%lu\n", rc, GetLastError());
                    }
                }
            }

            // 再试一次：用 WriteFile 写（某些设备对 HidD_SetOutputReport 不响应）
            printf("\n  >>> 改用 WriteFile 发送（ReportID=4）<<<\n");
            memset(wbuf, 0, sizeof(wbuf));
            wbuf[0] = 4; wbuf[3] = 0x1A; wbuf[4] = 0x06;
            OVERLAPPED ov;
            memset(&ov, 0, sizeof(ov));
            ov.hEvent = CreateEventW(0, TRUE, FALSE, 0);
            DWORD written = 0;
            BOOL wok = WriteFile(h, wbuf, (DWORD)outLen, &written, &ov);
            if (!wok && GetLastError() == ERROR_IO_PENDING)
            {
                if (WaitForSingleObject(ov.hEvent, 500) == WAIT_OBJECT_0)
                {
                    DWORD dw = 0;
                    wok = GetOverlappedResult(h, &ov, &dw, FALSE);
                    written = dw;
                }
            }
            printf("  WriteFile = %s, written=%lu, err=%lu\n",
                   wok ? "OK" : "FAIL", written, GetLastError());
            CloseHandle(ov.hEvent);

            memset(rbuf, 0, sizeof(rbuf));
            int rc2 = ReadWithTimeout(h, rbuf, (DWORD)inLen, 600);
            if (rc2 > 0) HexDump("    收到回包", rbuf, rc2);
            else printf("    读超时/失败 rc=%d\n", rc2);
        }

        printf("\n");
        HidD_FreePreparsedData(ppd);
        CloseHandle(h);
        free(dbuf);
    }

    SetupDiDestroyDeviceInfoList(di);
    printf("=== 探针结束 ===\n");
    return 0;
}
