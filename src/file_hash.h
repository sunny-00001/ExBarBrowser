// 文件摘要（tool.hash 的数据源，tool.dupfinder / tool.compare 复用）。
//
// 为什么单独一个文件：
//   · 它不碰 shell/COM，纯文件读 + BCrypt，可以脱离窗口单测；
//   · 它有两条容易写错的硬约束（见下），值得集中在一处说清楚。
//
// 两条硬约束：
//   1. **必须流式读**：几百 MB 的 iso 一次读进内存会把进程撑爆。
//      这里固定 64 KB 块，内存占用与文件大小无关。
//   2. **必须能中途停**：调用方在 UI 线程上跑（定时器泵 AI 请求 / 人按快捷键），
//      没有取消点就是把整个消息循环交给一个文件。取消靠调用方给的原子标志 +
//      每块检查一次，粒度足够细。
//
// 算法选择用 BCrypt（系统自带，不加第三方依赖）：MD5 / SHA-1 / SHA-256。
// 不提供 SHA-512 的别名映射之外的猜测 —— 认不出的名字就报错，不静默退回 SHA-256，
// 否则"我算的是 SHA-512"和"实际得到 SHA-256"无法区分，比报错更糟。
#pragma once
#include <atomic>
#include <string>

namespace filehash {

enum class Algo { Md5, Sha1, Sha256 };

// 界面/结果里显示的规范名：L"MD5" / L"SHA-1" / L"SHA-256"
const wchar_t* AlgoName(Algo a);

// 解析用户/agent 给的名字。接受：md5 / sha1 / sha-1 / sha256 / sha-256（大小写不敏感）。
// 认不出返回 false，**不改动** out。
bool ParseAlgo(const std::wstring& s, Algo* out);

struct Result {
    bool ok = false;
    std::wstring hex;                 // 小写十六进制
    unsigned long long bytes = 0;     // 实际读到的字节数（用于校验没读半截）
    std::wstring error;               // ok=false 时的人可读原因（含 L"已取消"）
};

// 计算 path 的摘要。path 是目录、不存在、被独占、cancel 被置位 → 返回 false 并写 error。
bool HashFile(const std::wstring& path, Algo algo, Result* out,
              const std::atomic<bool>* cancel = nullptr);

}  // namespace filehash
