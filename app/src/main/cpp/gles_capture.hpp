#pragma once
// GLES 顶点缓冲捕获。
//
// Vulkan 那条线已经证实走不通：游戏虽然加载了 libvulkan.so，但 50 秒内一个 buffer 都没创建，
// 渲染走的是 GLESv3。顶点数据从 CPU 到 GPU 的必经之路就变成了 glBufferData / glBufferSubData。
//
// 这两个函数在 libGLESv2.so 里是**导出**的（不像 Unity 那些内部符号），可以直接 dlsym 定位再挂钩。
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <tuple>

namespace ms::gles {

struct Attrib {uint32_t index=0,size=0,type=0,normalized=0,stride=0; uintptr_t offset=0;};

struct Blob {
    uint64_t seq = 0;
    uint64_t handle = 0;         // 上传时绑定的 GL buffer，和 draw 表对得上
    uint32_t bytes = 0;          // 实际留存字节数
    uint64_t declared = 0;       // 调用时报的 size（可能远大于留存量）
    std::vector<uint8_t> data;
    std::vector<Attrib> layout;  // 上传这一刻的 attrib 配置，缓冲对应的真实布局
};

struct Stats {
    uint64_t arrayUploads = 0;   // GL_ARRAY_BUFFER 上传次数
    uint64_t indexUploads = 0;   // GL_ELEMENT_ARRAY_BUFFER 上传次数
    uint64_t otherUploads = 0;
    uint64_t totalBytes = 0;
    bool installed = false;
    std::string detail;
};

// 等 libGLESv2.so 出现后挂钩 glBufferData / glBufferSubData。返回是否成功。
bool install();

// 只读统计，供后台线程打印。
Stats stats();

// 取走已捕获的顶点缓冲（swap，O(1)）。
std::vector<Blob> take();

// 当前累积的属性声明（最近 64 条，去重）
std::vector<Attrib> attribs();

// 写权限验证：命中多少次数（0 表示那个尺寸的缓冲没出现）
uint64_t patchHits();

// 批量试探命中的缓冲尺寸，按顺序
std::vector<int64_t> batchHits();

// 从客户端内存指针读到的顶点缓冲数
int clientCaptured();

// seq -> (handle, bytes)，拿到就能和 gles-draws.txt 的 handle 对上
std::vector<std::tuple<uint64_t,uint64_t,uint32_t>> blobMap();

// 每个顶点缓冲被画了几次（= 子网格数），指纹的第二个数字
std::vector<std::pair<uint64_t,uint32_t>> drawCounts();

}  // namespace ms::gles
