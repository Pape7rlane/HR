#pragma once
// il2cpp API 表探测（崩铁专用的一条捷径）。
//
// libil2cpp.so 只导出 il2cpp_get_api_table 一个函数，其余符号全是 32 位十六进制假名。但那个函数是导出的，
// 运行时 dlsym 就能拿到，不用理解它内部那堆 MOVZ/MOVK 混淆。它返回的是一张函数指针表——如果里面有
// il2cpp_class_from_name / il2cpp_runtime_invoke 这一批，整条原生 RVA 路子都可以不要。
//
// 这个模块只做一件事：把它调一次，把表头、每个指针的落点、以及每个函数开头几条指令写成文本文件。
// 不改任何东西，不挂钩，不依赖地址表——所以空表状态下也能跑，正好用来判断下一步走哪条路。
#include <string>

namespace ms::probe {
// 写到 path。没有 libil2cpp.so、没有这个导出、或者调用返回 0 都会抛 std::runtime_error 说明原因。
// 只读：调用 il2cpp_get_api_table() 本身不解引用任何参数。
void apiTable(const std::string& path, int round = 1);

// 在进程内存里找明文 global-metadata.dat（magic AF 1B B1 FA），找到就写到 outPath。
// 磁盘上那份是 MHY\0 加密的，但 il2cpp 要用它就必须先解密进内存——直接拿内存里的那份，
// 不用去啃解密循环。返回 dump 出来的字节数，0 表示没找到。
size_t memScanMetadata(const std::string& outPath);

// 探测 libunity.so 的导出符号：这批是真实导出的（_s_SkinVertices* / _s_TransformVertices*），
// 它们是"函数指针变量"，取到值就是 Unity 顶点处理/蒙皮的真实入口。
// 这条路不需要 il2cpp metadata，是绕开数据加密的备选方案。只读，不挂钩。
size_t probeUnitySymbols(const std::string& outPath);

// 给蒙皮函数挂钩子，只计数、不改参数、原样转发。用来判断游戏走的是 CPU 蒙皮还是 GPU 蒙皮。
// 返回成功挂上的个数。游戏如果因为签名不对崩掉，说明这批函数不是这个调用约定，回退即可。
size_t hookSkinning();
uint64_t skinCallCount();
// 每个函数各自的调用次数
std::string slotCallReport();
// 打开顶点采样：下一次 hook 命中时把前若干个顶点和骨骼矩阵记下来，写到 outPath。
void startVertexCapture(const std::string& outPath);
bool vertexCaptureDone();
// 由普通线程调用，把采样结果写成文本；没有样本返回 false。
bool writeVertexSample(const std::string& outPath);
}
