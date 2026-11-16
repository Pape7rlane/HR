#pragma once
// 崩铁（Honkai: Star Rail）的地址表。
//
// HSRBox 沿用 GIBox 的做法：不碰绘制，直接在托管对象 → 原生 Mesh / Texture 这一层替换。要做到这一点，需要一批
// Unity 引擎函数在游戏自己那个 so 里的地址。地址随游戏版本（Build ID）变，所以这张表是"每个版本一份"的。
//
//   默认状态：一张表都没有。模块照样加载，状态写 offsets-pending，什么都不挂钩，也不会崩。
//   适配某个版本：把该版本的 libil2cpp.so（单库构造时是游戏自己的 so）从 APK 里取出来，跑
//     python3 tools/scan_game.py libil2cpp.so --emit app/src/main/cpp/generated_offsets.inc
//   生成的 generated_offsets.inc 会被下面的 #include 收进来，重编一次这个版本就能用。
//
// 表里每一项都做了两重校验：地址必须落在可执行段并且 4 字节对齐，以及（有录制字节的项）开头 16 字节必须与
// 录制的一致——ADRP / ADR / B / BL / 条件分支 / 字面量装载 / 非 SP 基址的 load-store 立即数会随地址漂移，
// 比较时按 bootstrap.cpp 的 codeMask 屏蔽掉。校验不过就跳过这一项，不挂钩。
#include <algorithm>
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
namespace ms::abi {
// 这个进程实际加载的两个库的 Build ID（select() 填入）。
inline std::string buildId;          // libunity.so —— 地址表的 key
inline std::string managedBuildId;   // libil2cpp.so —— 托管侧项要用它核对
// 非空表示本版本还没适配完（或完全没适配）；bootstrap 看到它就不挂钩、不调用，只写状态。
inline std::string pendingText;      // 给人看的待定说明
inline std::string noteText;         // 表里自带的说明（Profile::pending）
inline const char* pendingRequired="";
// 引擎分配器给蒙皮权重块用的开关。默认开：走 MemoryManager::Reallocate；关掉时复用 mesh 自己的 skin 块。
inline bool allocVerified=true;
// MemoryManager 的"已初始化"字节，Allocate 只有在它是 1 时才走 label 的分配器。
inline uintptr_t managerActive=0xc4e;
// 实体管理器：*(base + manager) + managerSlot。
inline uintptr_t managerSlot=0x0;
// Entity 上的可视化组件字段、那个组件里 SkinnedMeshRenderer[] 的字段。
inline uintptr_t entityVisual=0x0,visualRenderers=0x0;
// 同一类型字段在版本间被重排时，dump 分不出来，运行时按"里面装的是什么"从这些候选里挑（Runtime::calibrate）。
inline std::vector<uint16_t> managerLists={};
inline std::vector<uint16_t> managerIds={};
inline std::vector<uint16_t> entityIds={};
// 校准完成后的实际字段偏移（0 表示还没定），给诊断用。
inline std::atomic<uint16_t> liveConfig{0},liveRuntime{0};
inline std::atomic<uint16_t> liveRoot{0};
#ifdef HSRBOX_HOST_TEST
inline constexpr uintptr_t notFound=0x15fff000;   // 宿主测试：落在它们的假镜像里，at() 也照常回答
#else
inline constexpr uintptr_t notFound=0xfffff000;
#endif
inline constexpr bool found(uintptr_t rva){return rva!=0&&rva<notFound;}
// UI.Text.m_Text（Text 画的那个字符串）在托管对象里的偏移。
inline constexpr uintptr_t uiTextString=0xd8;

// 需要解析的每一项。值本身不重要，表里用它的序号做 key，所以增删只能往末尾加。
enum Offset : uintptr_t {
 tick,manager,
 getMesh,setMesh,getBones,getMaterials,setMaterials,
 clone,destroy,getName,getParent,
 arrayNew,objectNew,pin,unpin,
 setReadable,setIndexFormat,getBindposes,getPositions,setPositions,setNormals,
 getUV,setUV,setUV2,setUV3,setUV4,getColors,setColors,getTangents,setTangents,
 getTriangles,setTriangles,setSubmeshCount,setSubmeshTriangles,meshFinish,meshUpload,
 meshDynamic,frameCount,
 textureType,textureInit,texturePixels,textureApply,
 setHideFlags,getHideFlags,
 mainTexture,newString,hasProperty,setTexture,
 getTexture,getShader,getTextureIds,propertyToId,
 nativeAlloc,nativeAllocator,
 findObjectsOfType,findObjectsNative,
 componentsInChildren,gameObject,
 uiTextSet,uiTextPopulate,uiVertexClear,behaviourEnabled,
 componentByName,lodPause,lodResume,lodSelfPaused,lodCurrent,
 receive,
 lodBind,meshBones,
 wrapperFor,
 typeObject,classFromType,
 meshSubmeshes,meshVertices,meshReadable,meshIndexStart,meshIndexCount,
 textureWidth,textureHeight,textureFormat,textureMips
};
inline constexpr std::size_t offsetCount=std::size_t(textureMips)+1;
inline const char* const offsetNames[offsetCount]={
 "tick","manager",
 "getMesh","setMesh","getBones","getMaterials","setMaterials",
 "clone","destroy","getName","getParent",
 "arrayNew","objectNew","pin","unpin",
 "setReadable","setIndexFormat","getBindposes","getPositions","setPositions","setNormals",
 "getUV","setUV","setUV2","setUV3","setUV4","getColors","setColors","getTangents","setTangents",
 "getTriangles","setTriangles","setSubmeshCount","setSubmeshTriangles","meshFinish","meshUpload",
 "meshDynamic","frameCount",
 "textureType","textureInit","texturePixels","textureApply",
 "setHideFlags","getHideFlags",
 "mainTexture","newString","hasProperty","setTexture",
 "getTexture","getShader","getTextureIds","propertyToId",
 "nativeAlloc","nativeAllocator",
 "findObjectsOfType","findObjectsNative",
 "componentsInChildren","gameObject",
 "uiTextSet","uiTextPopulate","uiVertexClear","behaviourEnabled",
 "componentByName","lodPause","lodResume","lodSelfPaused","lodCurrent",
 "receive",
 "lodBind","meshBones",
 "wrapperFor",
 "typeObject","classFromType",
 "meshSubmeshes","meshVertices","meshReadable","meshIndexStart","meshIndexCount",
 "textureWidth","textureHeight","textureFormat","textureMips"
};
inline const char* nameOf(Offset o){std::size_t i=std::size_t(o);return i<offsetCount?offsetNames[i]:"?";}

// 崩铁的引擎和托管分成两个 so，所以每个 Offset 要先说清归谁：查表、拼基址、校验入口字节都按这个走。
// 原神那种单库构造（libyuanshen.so）里两库基址相同，分类退化成"无所谓"。
enum class Lib : uint8_t { Engine, Managed };
inline constexpr Lib libOf(Offset o){
 switch(o){
  // 仅这 5 项是 libunity.so 里的 C++ 原生函数：引擎分配器、原生查找、LOD 切网格、BindMeshBones。
  case nativeAlloc: case nativeAllocator: case findObjectsNative:
  case lodBind: case meshBones:
   return Lib::Engine;
  // 其余全部是托管方法，编译在 libil2cpp.so 里。
  // 崩铁的 Unity 托管程序集（UnityEngine.CoreModule 等）和游戏程序集一起编进 libil2cpp.so，
  // 所以 Mesh/Renderer/Material/Texture/Object 这些 C# 方法、以及 il2cpp 运行时入口都在这一侧。
  // （原神是单库构造，两库基址相同，这个分类不产生影响；崩铁必须分对，否则基址会配错。）
  default: return Lib::Managed;
 }
}
inline const char* libName(Lib l){return l==Lib::Managed?"libil2cpp.so":"libunity.so";}

// 已录制的入口字节：挂钩前逐条比对，不过就不挂。
struct CodeCheck {Offset rva;const char* hex;};
inline CodeCheck handoffChecks[]={{componentsInChildren,""},{gameObject,""},{receive,""}};
inline CodeCheck bindChecks[]={{lodBind,""},{meshBones,""}};

// 一个游戏版本的地址表。
struct Profile {
 const char* buildId;                                  // libunity.so 的 GNU Build ID（地址表的 key）
 const char* managedBuildId;                           // libil2cpp.so 的 GNU Build ID；空 = 单库构造，两套基址相同
 const char* pending;                                  // 非空表示这一版还有项没找到，内容是说明
 uintptr_t managerActive,managerSlot,entityVisual,visualRenderers;
 std::vector<uint16_t> managerLists,managerIds,entityIds;
 std::vector<std::pair<Offset,uintptr_t>> rvas;        // 每项在该版本里的 RVA
 std::vector<std::pair<Offset,const char*>> checks;    // 要校验的项及其开头 16 字节
};

// 扫描器写出来的表；没生成时这里是空的，at() 全部返回 notFound，模块进 offsets-pending。
#ifdef HSRBOX_GENERATED_OFFSETS
#include "generated_offsets.inc"
#elif defined(HSRBOX_HOST_TEST)
// 宿主测试用的合成表：只为验证"按 Build ID 选表、未列出的项落 notFound、入口字节随表切换"。
inline std::vector<Profile>& profiles(){
 static std::vector<Profile> v{Profile{"hosttestbuildid000000000000000000000000","hostmanagedid0000000000000000000000","",0xc4e,0x90e0,0xc8,0x68,{},{},{},
  {{getMesh,0x1000},{manager,0x2000},{receive,0x3000},{lodBind,0x4000},{meshBones,0x4100}},
  {{receive,"fe4fbfa968ff0590e50304aae403032a"}}}};
 return v;}
#else
inline std::vector<Profile>& profiles(){static std::vector<Profile> v;return v;}
#endif

// 当前选中的表（select() 填）。空表示没有表。
inline std::vector<std::pair<uintptr_t,uintptr_t>>& table(){static std::vector<std::pair<uintptr_t,uintptr_t>> t;return t;}
inline uintptr_t at(Offset o){
 auto& t=table();
#ifdef HSRBOX_HOST_TEST
 if(t.empty())return uintptr_t(o);   // 宿主测试：表为空时用序号当偏移，测试自己按同样规则摆布局
#else
 if(t.empty())return notFound;
#endif
 std::size_t lo=0,hi=t.size();
 while(lo<hi){size_t m=(lo+hi)/2;if(t[m].first<uintptr_t(o))lo=m+1;else hi=m;}
 return lo<t.size()&&t[lo].first==uintptr_t(o)?t[lo].second:notFound;
}

// 缺项用的空桩：libil2cpp 里一条 ret 指令的 RVA（0x19441094 处 `ret`，已核对字节 c0035fd6）。
// 缺项没有被安排真地址时，addr() 返回 managedBase+retStubRva，调过去立刻返回，不会算出野指针。
inline constexpr uintptr_t retStubRva=0x19441094;
// 缺项清单（给人看）。缺项不再阻断模块：先让模块跑起来，由日志告诉我们哪一项真的被用到。
inline std::string stubText;
inline void resetPending(){
 pendingText=noteText;
 std::string missing;
 for(std::size_t i=0;i<offsetCount;i++)if(!found(at(Offset(i)))){if(!missing.empty())missing+=", ";missing+=offsetNames[i];}
 stubText=missing;
 if(!missing.empty())pendingText+=(pendingText.empty()?"":"；")+std::string("以下项用 ret 空桩占位（不会崩，但对应功能不可用）：")+missing;
 pendingRequired="";   // 不再阻断：缺项由空桩兜底
}

// 按实际加载的两个 Build ID 选表（引擎库 + 托管库）。没有这一版的表不算错误：
// 模块加载、状态写清楚、什么都不挂钩。
inline bool select(const std::string& id,const std::string& managed=""){
 buildId=id;managedBuildId=managed;
 table().clear();
 noteText.clear();
 for(const Profile& p:profiles()){
  if(id!=p.buildId)continue;
  noteText=p.pending?p.pending:"";
  managerActive=p.managerActive;managerSlot=p.managerSlot;
  entityVisual=p.entityVisual;visualRenderers=p.visualRenderers;
  managerLists=p.managerLists;managerIds=p.managerIds;entityIds=p.entityIds;
  // 托管库对不上时只保留引擎侧：托管那十几项（objectNew / arrayNew / typeObject …）留着会拿错基址
  const bool hasManaged=p.managedBuildId&&*p.managedBuildId;
  bool managedOk=hasManaged;
  if(hasManaged&&!managed.empty()&&managed!=p.managedBuildId){
   managedOk=false;
   noteText+=(noteText.empty()?"":"；")+std::string("libil2cpp.so 实际是 ")+managed+"，表里是 "+p.managedBuildId+"，托管侧项先不启用";
  }
  for(auto& r:p.rvas){
   if(hasManaged&&!managedOk&&libOf(r.first)==Lib::Managed)continue;
   table().push_back({uintptr_t(r.first),r.second});
  }
  std::sort(table().begin(),table().end());
  for(auto& c:handoffChecks)for(auto& x:p.checks)if(x.first==c.rva)c.hex=x.second;
  for(auto& c:bindChecks)for(auto& x:p.checks)if(x.first==c.rva)c.hex=x.second;
  resetPending();
  return true;
 }
 noteText="本版本（"+id+"）还没有地址表";
 resetPending();
 return true;
}
}  // namespace ms::abi
