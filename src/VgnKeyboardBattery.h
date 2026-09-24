// ===========================================================================
//  VgnKeyboardBattery.h
//  TrafficMonitor 插件：在任务栏/主窗口显示 VGN 键盘电量
// ===========================================================================
#pragma once

#include "PluginInterface.h"
#include "VgnHidReader.h"

// ---------------------------------------------------------------------------
//  显示项目：键盘电量
// ---------------------------------------------------------------------------
class CKeyboardBatteryItem : public IPluginItem
{
public:
    CKeyboardBatteryItem();

    // 由插件在 DataRequired() 中调用，刷新显示文本
    void Update(bool scanned, bool found, bool online, bool charging, int level);

    // ---- IPluginItem ----
    virtual const wchar_t* GetItemName() const override;
    virtual const wchar_t* GetItemId() const override;
    virtual const wchar_t* GetItemLableText() const override;
    virtual const wchar_t* GetItemValueText() const override;
    virtual const wchar_t* GetItemValueSampleText() const override;
    virtual int IsDrawResourceUsageGraph() const override;
    virtual float GetResourceUsageGraphValue() const override;

private:
    wchar_t m_value[32];    // 当前显示文本
    float   m_graph;        // 资源占用图的值 0.0~1.0（用来画电量条）
};

// ---------------------------------------------------------------------------
//  插件本体
// ---------------------------------------------------------------------------
class CVgnKeyboardBatteryPlugin : public ITMPlugin
{
public:
    static CVgnKeyboardBatteryPlugin& Instance();

    // ---- ITMPlugin ----
    virtual IPluginItem* GetItem(int index) override;
    virtual void DataRequired() override;
    virtual const wchar_t* GetInfo(PluginInfoIndex index) override;
    virtual const wchar_t* GetTooltipInfo() override;
    virtual int GetCommandCount() override;
    virtual const wchar_t* GetCommandName(int command_index) override;
    virtual void OnPluginCommand(int command_index, void* hWnd, void* para) override;
    virtual void OnInitialize(ITrafficMonitor* pApp) override;

private:
    CVgnKeyboardBatteryPlugin();

    VgnHidReader         m_reader;
    CKeyboardBatteryItem m_item;
    wchar_t              m_tooltip[256];
};

// ---------------------------------------------------------------------------
//  插件入口
// ---------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif
    __declspec(dllexport) ITMPlugin* TMPluginGetInstance();
#ifdef __cplusplus
}
#endif
