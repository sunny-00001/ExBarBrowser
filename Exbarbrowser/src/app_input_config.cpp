// App 侧 InputConfig 实现：把输入端的配置读写转发到 Config 单例。
//
// 【为什么单独一个 TU】app.cpp 已在行数棘轮边界上（917 记账）。这段是纯粹的
// "接口适配"（InputConfig 的虚方法 → Config 的静态访问），与 App 的业务编排
// 无关，单开一个文件既守住行数，也让"输入端配置边界"这一层可见。
// 依赖方向：app → config（下层），符合单向；输入端只见 InputConfig 抽象（R15）。
#include "input_config.h"

#include "config.h"

class AppInputConfig final : public InputConfig {
public:
    std::wstring KeymapPath() const override { return Config::Get().KeymapPath(); }
    std::wstring MousePath() const override { return Config::Get().MousePath(); }
    bool SeqUniqueAuto() const override { return Config::Get().s().seqUniqueAuto; }
    bool SeqSingleInstant() const override { return Config::Get().s().seqSingleInstant; }
    bool SeqEnabled() const override { return Config::Get().s().seqEnabled; }
    bool SeqShowHints() const override { return Config::Get().s().seqShowHints; }
    int SeqTimeoutMs() const override { return Config::Get().s().seqTimeoutMs; }
    int HoldDelayMs() const override { return Config::Get().s().holdDelayMs; }
    const std::vector<std::pair<std::wstring, std::wstring>>& HotkeyOverrides() const override {
        return Config::Get().s().hotkeyOverrides;
    }

    void SetHotkeyOverride(const std::wstring& setting, const std::wstring& key) override {
        Config::Get().SetHotkeyOverride(setting, key);
    }
    void ClearAllHotkeyOverrides() override {
        Config::Get().mutableSettings().hotkeyOverrides.clear();
    }
    void Save() override { Config::Get().Save(); }
};

// 供 App 持有的单例式适配器（无状态，一个进程一份）。
InputConfig& AppInputConfigInstance() {
    static AppInputConfig instance;
    return instance;
}
