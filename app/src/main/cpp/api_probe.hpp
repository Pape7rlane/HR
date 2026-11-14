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
}
