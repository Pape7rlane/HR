#include "runtime.hpp"
#include "json.hpp"
#include "logfmt.hpp"
#include "logic_import.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <jni.h>
#include <link.h>
#include <iterator>
#include <thread>
#include "dobby.h"
#include "vk_capture.hpp"
#include "api_probe.hpp"
#include "gles_capture.hpp"
#include "uid_hide.hpp"

namespace {
std::atomic<bool> started{false};std::unique_ptr<ms::Runtime> runtime;
std::atomic<ms::Runtime*> published{nullptr};
// Process lifetime: partial hook installation still needs its passthroughs.
uint64_t now(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void onTick(void*,DobbyRegisterContext*){
 if(auto* p=published.load(std::memory_order_acquire))p->tick(now());
}
// The game's async model hand-off (abi::receive): attach before its own callback, check after (Runtime::spawned).
using ReceiveFn=void(*)(uintptr_t,uint64_t,bool,uint32_t,uintptr_t,uintptr_t);
ReceiveFn receiveOriginal=nullptr;
void onReceive(uintptr_t self,uint64_t key,bool ok,uint32_t handle,uintptr_t asset,uintptr_t method){
 auto* p=published.load(std::memory_order_acquire);const bool handled=p&&ok&&asset;
 if(handled)p->spawned(asset);
 receiveOriginal(self,key,ok,handle,asset,method);
 if(handled)p->spawnedAfter();
}
// The game's own mesh + bone binding: the mesh argument is swapped before the original runs (Runtime::bindMesh). Up to
// eight integer arguments and x0 are passed through untouched; neither entry takes floating-point arguments.
using BindFn=uint64_t(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
BindFn lodBindOriginal=nullptr,meshBonesOriginal=nullptr;
uint64_t onLodBind(uintptr_t loader,uintptr_t mesh,uintptr_t renderer,uintptr_t a3,uintptr_t a4,uintptr_t a5,uintptr_t a6,uintptr_t a7){
 if(auto* p=published.load(std::memory_order_acquire))mesh=p->bindMesh(renderer,mesh);
 auto result=lodBindOriginal(loader,mesh,renderer,a3,a4,a5,a6,a7);
 if(auto* p=published.load(std::memory_order_acquire))p->meshBound(renderer);
 return result;}
uint64_t onMeshBones(uintptr_t renderer,uintptr_t mesh,uintptr_t hashes,uintptr_t bones,uintptr_t a4,uintptr_t a5,uintptr_t a6,uintptr_t a7){
 if(auto* p=published.load(std::memory_order_acquire))mesh=p->bindMesh(renderer,mesh);
 auto result=meshBonesOriginal(renderer,mesh,hashes,bones,a4,a5,a6,a7);
 if(auto* p=published.load(std::memory_order_acquire))p->meshBound(renderer);
 return result;}
// UID hiding (uid_hide.hpp): Text.set_text runs, then a Text that now shows a UID label is switched off; a Text laid
// out with one draws nothing. Neither changes the string; ms::uid::on is the live switch.
using TextSetFn=void(*)(uintptr_t,uintptr_t,uintptr_t);using PopulateFn=void(*)(uintptr_t,uintptr_t,uintptr_t);
using ClearFn=void(*)(uintptr_t,uintptr_t);using EnabledFn=void(*)(uintptr_t,bool,uintptr_t);
TextSetFn textSetOriginal=nullptr;PopulateFn populateOriginal=nullptr;ClearFn vertexClear=nullptr;EnabledFn setEnabled=nullptr;
std::atomic<unsigned> uidHidden{0};
void onTextSet(uintptr_t text,uintptr_t value,uintptr_t method){
 const bool hide=ms::uid::on.load(std::memory_order_relaxed)&&ms::uid::managedUid(value);
 textSetOriginal(text,value,method);
 if(hide&&text){setEnabled(text,false,0);uidHidden.fetch_add(1,std::memory_order_relaxed);}}
void onPopulate(uintptr_t text,uintptr_t helper,uintptr_t method){
 if(text&&helper&&ms::uid::on.load(std::memory_order_relaxed)&&ms::uid::managedUid(*reinterpret_cast<const uintptr_t*>(text+ms::abi::uiTextString))){vertexClear(helper,0);uidHidden.fetch_add(1,std::memory_order_relaxed);return;}
 populateOriginal(text,helper,method);}
// The recorded entry bytes (7.1) against this build's: equal, except the immediates that move with the code's address
// (ADRP / ADR pages, B / BL / conditional branch offsets, literal loads, and the low 12 bits of a load / store / add
// off a register other than SP: an ADRP's page offset). 7.2's get_gameObject and resource receiver differ only there.
uint32_t codeMask(uint32_t w){
 if((w&0x9F000000)==0x90000000||(w&0x9F000000)==0x10000000)return 0x9F00001F;
 if((w&0x7C000000)==0x14000000)return 0xFC000000;
 if((w&0xFF000010)==0x54000000||(w&0x7E000000)==0x34000000)return 0xFF00001F;
 if((w&0x7E000000)==0x36000000)return 0xFFF8001F;
 if((w&0x3B000000)==0x18000000)return 0xFF00001F;
 if((w&0x3B000000)==0x39000000&&((w>>5)&31)!=31)return 0xFFC003FF;
 if((w&0x7F800000)==0x11000000&&((w>>5)&31)!=31)return 0xFFC003FF;
 return 0xFFFFFFFF;}
bool codeMatches(uintptr_t base,const ms::abi::CodeCheck& c){if(!c.hex||!*c.hex)return true;   // 这一版没录入口字节：只做地址检查
 auto rva=ms::abi::at(c.rva);if(!ms::abi::found(rva))return false;auto* p=reinterpret_cast<const unsigned char*>(base+rva);auto digit=[](char x){return x<='9'?x-'0':x-'a'+10;};
 for(size_t i=0;i<16;i+=4){uint32_t want=0,got=0;for(size_t b=0;b<4;b++){want|=uint32_t(digit(c.hex[2*(i+b)])*16+digit(c.hex[2*(i+b)+1]))<<(8*b);got|=uint32_t(p[i+b])<<(8*b);}
  uint32_t m=codeMask(want);if((want&m)!=(got&m))return false;}
 return true;}
struct Image{uintptr_t base=0;const ElfW(Phdr)* ph=nullptr;size_t count=0;std::string name;int rank=999;};
// 崩铁 Android 端引擎与 il2cpp 的布局两种都可能：拆开的 libil2cpp.so + libunity.so，或者单库构造（miHoYo 自定制的
// Unity 导出）。这里按优先级收第一个已加载的候选，实际用的是哪个写进启动日志，靠 Image::name 传给后面的状态输出。
// 崩铁实测（国际服 4.6.54）：引擎在 libunity.so（47 MB，.text 35 MB），托管层在 libil2cpp.so（460 MB，.text 363 MB）。
// 跟原神 libyuanshen.so 的单库融合不一样，这里要两套基址：
//   Mesh / Texture / Material / SkinnedMeshRenderer / LOD 这些原生入口 → libunity.so（地址表按它的 Build ID 走）
//   objectNew / arrayNew / typeObject / nativeAlloc 这些 il2cpp 侧 → libil2cpp.so
// 先按 libunity.so 找引擎基址；找不到再退回单库构造去认 libil2cpp.so。
const char* const kEngineLibs[]={"libunity.so","libil2cpp.so","libhkrpg.so","libGame.so"};
constexpr size_t kEngineLibCount=sizeof(kEngineLibs)/sizeof(kEngineLibs[0]);
int findImage(dl_phdr_info* info,size_t,void* ptr){std::string name=info->dlpi_name?info->dlpi_name:"";auto p=name.find_last_of('/');name=name.substr(p==name.npos?0:p+1);
 auto& out=*static_cast<Image*>(ptr);
 for(size_t i=0;i<kEngineLibCount;i++){if(name!=kEngineLibs[i])continue;
  if(out.base&&out.rank<=int(i))return 0;               // 更靠前的候选已经收下
  out={info->dlpi_addr,info->dlpi_phdr,info->dlpi_phnum,name,int(i)};return 1;}
 return 0;}
int findManaged(dl_phdr_info* info,size_t,void* ptr){std::string name=info->dlpi_name?info->dlpi_name:"";auto p=name.find_last_of('/');name=name.substr(p==name.npos?0:p+1);
 if(name!="libil2cpp.so")return 0;auto& out=*static_cast<Image*>(ptr);out={info->dlpi_addr,info->dlpi_phdr,info->dlpi_phnum,name,0};return 1;}
std::string jstringValue(JNIEnv* env,jstring s){ms::require(s!=nullptr,"Java 字符串为空");const char* p=env->GetStringUTFChars(s,nullptr);ms::require(p!=nullptr,"无法解码 Java 字符串");std::string out=p;env->ReleaseStringUTFChars(s,p);return out;}
void exception(JNIEnv* env,const std::exception& e){env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),e.what());}
std::string sharedDir(const std::string& pkg){return "/storage/emulated/0/Android/media/"+pkg+"/HSRBox";}
// Start-up lines go to the same "HSRBox Log.txt" as the runtime (logger "bootstrap", XXMI line format).
void bootLog(const std::string& pkg,const std::string& state,const std::string& detail=""){try{auto dir=sharedDir(pkg);auto level=ms::levelOf(state);if(int(level)<ms::logThresholdOf(dir))return;
 std::filesystem::create_directories(dir);ms::appendLog(dir,ms::logLine("bootstrap",level,detail.empty()?state:state+' '+detail));}catch(const std::exception&){}}
void bootStatus(const std::string& pkg,const std::string& state,const std::string& detail){auto dir=sharedDir(pkg);try{std::filesystem::create_directories(dir);auto s="{\"state\":"+ms::quote(state)+",\"detail\":"+ms::quote(detail)+",\"characterId\":0,\"model\":\"\"}";ms::write(dir+"/status.json.tmp",s);std::filesystem::rename(dir+"/status.json.tmp",dir+"/status.json");}catch(const std::exception&){}bootLog(pkg,state,detail);}
}
// 诊断：进程里到底加载了哪些模块。找不到 libil2cpp.so 时用它一次看清。
struct ModuleList{std::string text;size_t n=0;};
int collectModule(dl_phdr_info* info,size_t,void* p){
 auto& out=*static_cast<ModuleList*>(p);
 std::string n=info->dlpi_name?info->dlpi_name:"";
 auto s=n.find_last_of('/');std::string base=s==std::string::npos?n:n.substr(s+1);
 if(base.empty()||base[0]!='l'||base.compare(0,3,"lib"))return 0;   // 只要 lib*.so
 if(++out.n>80)return 1;
 out.text+=(out.text.empty()?"":" ")+base;
 return 0;}
void dumpModules(const std::string& pkg){ModuleList l;dl_iterate_phdr(collectModule,&l);
 bootLog(pkg,"modules","已加载 "+std::to_string(l.n)+" 个 lib*.so："+l.text);}

extern "C" JNIEXPORT void JNICALL Java_com_hsrbox_tool_NativeBridge_start(JNIEnv* env,jclass,jstring jp,jstring jd){try{
 auto pkg=jstringValue(env,jp),priv=jstringValue(env,jd);ms::require(pkg=="com.miHoYo.hkrpg"||pkg=="com.miHoYo.hkrpg.bilibili"||pkg=="com.HoYoverse.hkrpgoversea"||pkg=="com.HoYoverse.hkrpgoverseacbtest"||pkg=="com.HoYoverse.hkrpgoverseacbtest","不支持的包名");if(started.exchange(true))return;
 // The index-buffer hashes (vk_capture.hpp) hook the Vulkan loader now, before the game makes its device; only when
 // control.json textureMods.hashMode (or the older ibHash / capture switches) is on at this start (a change takes a restart).
 {auto mode=ms::vkcap::Mode::Off;try{auto c=ms::JsonParser(ms::text(sharedDir(pkg)+"/control.json")).parse();const auto& t=c.at("textureMods");
   if(t.type==ms::Json::Object){auto flag=[&](const char* k){return t.at(k).type==ms::Json::Bool&&t.at(k).boolean;};
    mode=ms::vkcap::modeFrom(t.at("hashMode").type==ms::Json::String?t.at("hashMode").string:std::string(),flag("ibHash"),flag("capture"));}}catch(const std::exception&){}
  // 顶点缓冲捕获：蒙皮走 GPU 之后，定点数据的唯一必经之路就是 Vulkan 上传。
  bool wantVertex=true;
  {try{auto c=ms::JsonParser(ms::text(sharedDir(pkg)+"/control.json")).parse();auto v=c.at("vertexCapture");if(v.type==ms::Json::Bool)wantVertex=v.boolean;}catch(const std::exception&){}}
  if(wantVertex&&mode==ms::vkcap::Mode::Off)mode=ms::vkcap::Mode::Collect;
  if(mode!=ms::vkcap::Mode::Off){ms::vkcap::tracker().allForms=mode==ms::vkcap::Mode::Full;std::string detail;bool ok=false;try{ok=ms::vkhook::install(detail);}catch(const std::exception& e){detail=e.what();}
   bootLog(pkg,ok?"vulkan-capture":"vulkan-capture-disabled",ok?"已安装："+detail:detail);
   if(ok&&wantVertex){
    std::thread([pkg]{for(int i=0;i<24;i++){std::this_thread::sleep_for(std::chrono::seconds(5));
      {auto c=ms::vkcap::tracker().counts();
       bootLog(pkg,"vk-buffers","第 "+std::to_string(i*2+2)+" 秒：buffer "+std::to_string(c.first)+" 个，其中 vertex "+std::to_string(c.second)+" 个");}
      auto blobs=ms::vkcap::tracker().takeVertex();
      if(blobs.empty())continue;
      std::string out="# HSRBox —— 顶点缓冲捕获\n";
      char b[256];
      for(auto& v:blobs){
        std::snprintf(b,sizeof b,"\nbuffer=%#zx offset=%#zx bytes=%u via=%s\n",(size_t)v.buffer,(size_t)v.offset,v.bytes,v.via.c_str());
        out+=b;
        size_t strides[3]={24,32,20};
        for(size_t st:strides){
          std::snprintf(b,sizeof b,"  stride %zu:\n",st);out+=b;
          for(int k=0;k<3;k++){
            size_t off=size_t(k)*st;
            if(off+24>v.data.size())break;
            float f0,f1,f2,f3,f4,f5;
            std::memcpy(&f0,v.data.data()+off,4);std::memcpy(&f1,v.data.data()+off+4,4);std::memcpy(&f2,v.data.data()+off+8,4);
            std::memcpy(&f3,v.data.data()+off+12,4);std::memcpy(&f4,v.data.data()+off+16,4);std::memcpy(&f5,v.data.data()+off+20,4);
            std::snprintf(b,sizeof b,"    v%d: %9.4f %9.4f %9.4f | %9.4f %9.4f %9.4f\n",k,(double)f0,(double)f1,(double)f2,(double)f3,(double)f4,(double)f5);
            out+=b;
          }
        }
      }
      try{ms::write(sharedDir(pkg)+"/vertex-buffers.txt",out);}catch(...){}
      bootLog(pkg,"vertex-capture","已写出 vertex-buffers.txt，"+std::to_string(blobs.size())+" 个缓冲");
      return;}}).detach();
   }
  }}
 std::thread([pkg,priv]{try{bootStatus(pkg,"waiting","等待游戏引擎库（libil2cpp.so / libunity.so）");Image image;for(unsigned n=0;n<480&&!image.base;n++){dl_iterate_phdr(findImage,&image);if(!image.base)std::this_thread::sleep_for(std::chrono::milliseconds(500));}ms::require(image.base!=0,"等待游戏引擎库加载超时（libil2cpp.so / libunity.so）");
 bootLog(pkg,"engine-lib","引擎库："+image.name);
 // GLES 顶点缓冲捕获：Vulkan 那条已证实不走（50 秒 0 个 buffer），渲染走 GLESv3。
 // glBufferData 在 libGLESv2.so 里是导出的，可以直接挂钩。
 {bool want=true;try{auto c=ms::JsonParser(ms::text(sharedDir(pkg)+"/control.json")).parse();auto v=c.at("glesCapture");if(v.type==ms::Json::Bool)want=v.boolean;}catch(const std::exception&){}
  if(want){
   std::thread([pkg]{
     if(!ms::gles::install()){bootLog(pkg,"gles-capture-failed",ms::gles::stats().detail);return;}
     bootLog(pkg,"gles-capture","已挂钩 "+ms::gles::stats().detail);
     size_t done=0;
     try{std::filesystem::create_directories(sharedDir(pkg)+"/gles-raw");}catch(...){}
     for(int i=0;i<75;i++){
       std::this_thread::sleep_for(std::chrono::seconds(2));
       auto st=ms::gles::stats();
       bootLog(pkg,"gles-stats","第 "+std::to_string(i*2+2)+" 秒：顶点上传 "+std::to_string(st.arrayUploads)+
               " 次，索引 "+std::to_string(st.indexUploads)+" 次，共 "+std::to_string(st.totalBytes/1024)+" KB");
       // 属性布局：游戏自己声明的 stride/offset，不用猜
       {auto at=ms::gles::attribs();
        if(!at.empty()){
          std::string lay="attrib 声明 "+std::to_string(at.size())+" 条：";
          for(auto& a:at){
            char b[160];
            const char* tn="?";
            switch(a.type){case 0x1400:tn="BYTE";break;case 0x1401:tn="UBYTE";break;case 0x1402:tn="SHORT";break;
              case 0x1403:tn="USHORT";break;case 0x1404:tn="INT";break;case 0x1405:tn="UINT";break;
              case 0x1406:tn="FLOAT";break;case 0x140B:tn="HALF";break;}
            std::snprintf(b,sizeof b," [%u] %ux%s stride=%u off=%#zx%s",a.index,a.size,tn,a.stride,(size_t)a.offset,a.normalized?" norm":"");
            lay+=b;
          }
          bootLog(pkg,"gles-layout",lay);
          try{ms::write(sharedDir(pkg)+"/gles-layout.txt",lay);}catch(...){}
        }}
       auto blobs=ms::gles::take();
       if(!blobs.empty())done+=blobs.size();
       {auto h=ms::gles::patchHits();
        if(h>0)bootLog(pkg,"gles-patch","已改写 25800 字节缓冲的首顶点 x+0.5（共 "+std::to_string(h)+" 次）");}
       if(blobs.empty())continue;
       // 存原始字节：步长和偏移得靠统计才能定，文本里 3 个顶点不够。
       for(auto& v:blobs){
         try{ms::write(sharedDir(pkg)+"/gles-raw/"+std::to_string(v.seq)+"_"+std::to_string(v.declared)+".bin",
                       v.data.data(),v.data.size());}catch(...){}
       }
       std::string out="# HSRBox —— GLES 顶点缓冲\n";
       char b[256];
       for(auto& v:blobs){
         std::snprintf(b,sizeof b,"\nseq=%llu 声明大小=%llu 留存=%u\n",(unsigned long long)v.seq,(unsigned long long)v.declared,v.bytes);
         out+=b;
         std::snprintf(b,sizeof b,"  布局:");out+=b;
         for(auto& a:v.layout){
           const char* tn="?";
           switch(a.type){case 0x1400:tn="BYTE";break;case 0x1401:tn="UBYTE";break;case 0x1402:tn="SHORT";break;
             case 0x1403:tn="USHORT";break;case 0x1404:tn="INT";break;case 0x1405:tn="UINT";break;
             case 0x1406:tn="FLOAT";break;case 0x140B:tn="HALF";break;}
           std::snprintf(b,sizeof b," [%u]%ux%s@%#zx",a.index,a.size,tn,(size_t)a.offset);out+=b;
         }
         out+="\n";
         if(!v.layout.empty()) std::snprintf(b,sizeof b,"  stride=%u\n",v.layout[0].stride),out+=b;
         size_t strides[3]={24,32,20};
         for(size_t st:strides){
           std::snprintf(b,sizeof b,"  stride %zu:\n",st);out+=b;
           for(int k=0;k<3;k++){
             size_t off=size_t(k)*st;
             if(off+24>v.data.size())break;
             float f0,f1,f2,f3,f4,f5;
             std::memcpy(&f0,v.data.data()+off,4);std::memcpy(&f1,v.data.data()+off+4,4);std::memcpy(&f2,v.data.data()+off+8,4);
             std::memcpy(&f3,v.data.data()+off+12,4);std::memcpy(&f4,v.data.data()+off+16,4);std::memcpy(&f5,v.data.data()+off+20,4);
             std::snprintf(b,sizeof b,"    v%d: %9.4f %9.4f %9.4f | %9.4f %9.4f %9.4f\n",k,(double)f0,(double)f1,(double)f2,(double)f3,(double)f4,(double)f5);
             out+=b;
           }
         }
       }
       try{ms::write(sharedDir(pkg)+"/gles-vertex.txt",out);}catch(...){}
       bootLog(pkg,"gles-capture-done","已写出 gles-vertex.txt，累计 "+std::to_string(done)+" 个缓冲");
     }
   }).detach();
  }}

 // 记录进程实际用了哪套图形 API：libvulkan 还是 libGLESv2/v3。走 GLES 的话 Vulkan 钩子一辈子都不会响。
 {struct G {std::string text;int n=0;};
  static G g;
  struct C {static int f(dl_phdr_info* i,size_t,void*) {
    std::string n=i->dlpi_name?i->dlpi_name:"";
    auto s=n.find_last_of('/');std::string b=s==std::string::npos?n:n.substr(s+1);
    if(b=="libvulkan.so"||b=="libGLESv2.so"||b=="libGLESv3.so"||b=="libEGL.so"||b=="libhwcpipe.so"){
      g.text+=(g.text.empty()?"":" ")+b; g.n++;}
    return 0;}};
  dl_iterate_phdr(C::f,nullptr);
  bootLog(pkg,"graphics-api",g.n?g.text:"（没看到 vulkan/GLES/EGL，可能还没加载）");}

 // 托管库单独找：崩铁的 il2cpp 侧偏移（objectNew / arrayNew / typeObject …）都在 libil2cpp.so 里。
 // Unity 先加载 libunity.so，il2cpp 要晚一步（实测启动 4 秒时还没进进程），所以这里必须等，不能看一眼就判死刑。
 Image managed;
 for(unsigned n=0;n<240&&!managed.base;n++){dl_iterate_phdr(findManaged,&managed);
  if(!managed.base)std::this_thread::sleep_for(std::chrono::milliseconds(500));}
 std::string managedId;
 if(managed.base){managedId=ms::loadedBuildId(managed.base,managed.ph,managed.count,sizeof(ElfW(Phdr)));
  bootLog(pkg,"managed-lib",managed.name+" Build ID "+managedId);}
 else{bootLog(pkg,"managed-lib","等了 120 秒仍没有 libil2cpp.so");dumpModules(pkg);}
 auto actual=ms::loadedBuildId(image.base,image.ph,image.count,sizeof(ElfW(Phdr)));
 if(!ms::abi::select(actual,managedId)){bootStatus(pkg,"buildid-mismatch","实际 Build ID："+actual);return;}
 // libunity.so 的导出符号探测：那批 _s_SkinVertices* / _s_TransformVertices* 是真实导出的函数指针，
 // 拿到就等于拿到顶点处理的入口，不需要 il2cpp metadata。只读，不挂钩。
 {bool want=true;try{auto c=ms::JsonParser(ms::text(sharedDir(pkg)+"/control.json")).parse();auto v=c.at("unitySymbols");if(v.type==ms::Json::Bool)want=v.boolean;}catch(const std::exception&){}
  if(want){try{const size_t n=ms::probe::probeUnitySymbols(sharedDir(pkg)+"/unity-symbols.txt");
       bootLog(pkg,"unity-symbols","可用 "+std::to_string(n)+" 个，已写出 unity-symbols.txt");}
   catch(const std::exception& e){bootLog(pkg,"unity-symbols-failed",e.what());}}}

 // 蒙皮函数计数：挂了才知道游戏走 CPU 蒙皮还是 GPU 蒙皮。只计数不改数据。
 {bool want=true;try{auto c=ms::JsonParser(ms::text(sharedDir(pkg)+"/control.json")).parse();auto v=c.at("skinProbe");if(v.type==ms::Json::Bool)want=v.boolean;}catch(const std::exception&){}
  if(want){try{const size_t n=ms::probe::hookSkinning();
       bootLog(pkg,"skin-hook","挂上 "+std::to_string(n)+" 个蒙皮函数，开始计数");
       std::thread([pkg]{for(int i=1;i<=6;i++){std::this_thread::sleep_for(std::chrono::seconds(10));
         bootLog(pkg,"skin-count","第 "+std::to_string(i*10)+" 秒累计 "+std::to_string(ms::probe::skinCallCount())+" 次 | "+ms::probe::slotCallReport());}}).detach();
       // 15 秒后采一次顶点样（只读），搞清顶点流布局
       std::thread([pkg]{std::this_thread::sleep_for(std::chrono::seconds(15));
         ms::probe::startVertexCapture(sharedDir(pkg)+"/vertex-sample.txt");
         for(int i=0;i<25;i++){std::this_thread::sleep_for(std::chrono::seconds(1));
           if(ms::probe::vertexCaptureDone()){
             if(ms::probe::writeVertexSample(sharedDir(pkg)+"/vertex-sample.txt"))
               bootLog(pkg,"vertex-sample","已写出 vertex-sample.txt");
             else bootLog(pkg,"vertex-sample","采样成功但写盘失败");
             return;}}
         bootLog(pkg,"vertex-sample","25 秒内没等到蒙皮调用（采样未触发）");}).detach();}
   catch(const std::exception& e){bootLog(pkg,"skin-hook-failed",e.what());}}}

 // 内存里找明文 global-metadata.dat：磁盘上那份是 MHY\0 加密的，il2cpp 要用就得先解密进内存，
 // 直接拿内存里那份，绕开解密逻辑。结果写到 HSRBox 目录下的 global-metadata.dat。
 {bool want=false;try{auto c=ms::JsonParser(ms::text(sharedDir(pkg)+"/control.json")).parse();auto v=c.at("memScan");if(v.type==ms::Json::Bool)want=v.boolean;}catch(const std::exception&){}
  if(want){std::this_thread::sleep_for(std::chrono::seconds(20));
   try{const size_t n=ms::probe::memScanMetadata(sharedDir(pkg)+"/global-metadata.dat");
       bootLog(pkg,"metadata-dump","已从内存 dump "+std::to_string(n)+" 字节到 global-metadata.dat");}
   catch(const std::exception& e){bootLog(pkg,"metadata-dump-failed",e.what());}}}

 // il2cpp API 表探测：不依赖地址表，空表也照样出结果。结果落在 api-table.txt，control.json 里 apiProbe:false 可关。
 // 放在待定检查之前——这条探测的意义就是决定后面走原生 RVA 还是托管调用。
 {bool want=false;try{auto c=ms::JsonParser(ms::text(sharedDir(pkg)+"/control.json")).parse();auto v=c.at("apiProbe");if(v.type==ms::Json::Bool)want=v.boolean;}catch(const std::exception&){}
  if(want){
   // 分三轮：il2cpp 可能还没初始化完，第一次调用崩不代表永远不行。每轮结果都追加进 api-table.txt。
   for(int round=1;round<=3;round++){
    std::this_thread::sleep_for(std::chrono::seconds(round==1?15:30));
    try{ms::probe::apiTable(sharedDir(pkg)+"/api-table.txt",round);
        bootLog(pkg,"api-probe","第 "+std::to_string(round)+" 轮成功，已写出 api-table.txt");break;}
    catch(const std::exception& e){bootLog(pkg,"api-probe-failed","第 "+std::to_string(round)+" 轮："+e.what());}}}}
 // UID hiding: hooked only when "hideUid" is on at this start. Each address must be inside an executable segment and
 // start a function body (not a B to elsewhere: hooking a 4-byte wrapper would write over the next method).
 {bool want=false;try{auto c=ms::JsonParser(ms::text(sharedDir(pkg)+"/control.json")).parse();want=c.at("hideUid").type==ms::Json::Bool&&c.at("hideUid").boolean;}catch(const std::exception&){}
  if(want){ms::uid::on.store(true);std::string bad;
   auto exec=[&](uintptr_t rva){if(!ms::abi::found(rva))return false;uintptr_t a=image.base+rva;for(size_t i=0;i<image.count;i++){auto& h=image.ph[i];if(h.p_type==PT_LOAD&&(h.p_flags&PF_X)&&a>=image.base+h.p_vaddr&&a-(image.base+h.p_vaddr)<h.p_memsz)return (rva&3)==0;}return false;};
   for(auto o:{ms::abi::uiTextSet,ms::abi::uiTextPopulate,ms::abi::uiVertexClear,ms::abi::behaviourEnabled}){auto rva=ms::abi::at(o);char b[24];std::snprintf(b,sizeof(b),"0x%llx ",static_cast<unsigned long long>(rva));
    if(!exec(rva))bad+=std::string(b)+"不在可执行段 ";else if(o==ms::abi::uiTextSet||o==ms::abi::uiTextPopulate){uint32_t w=*reinterpret_cast<const uint32_t*>(image.base+rva);if((w&0xfc000000u)==0x14000000u)bad+=std::string(b)+"是跳转 ";}}
   if(!bad.empty())bootLog(pkg,"uid-hide-disabled","地址检查未通过："+bad);
   else{setEnabled=reinterpret_cast<EnabledFn>(image.base+ms::abi::at(ms::abi::behaviourEnabled));vertexClear=reinterpret_cast<ClearFn>(image.base+ms::abi::at(ms::abi::uiVertexClear));
    bool a=DobbyHook(reinterpret_cast<void*>(image.base+ms::abi::at(ms::abi::uiTextSet)),reinterpret_cast<dobby_dummy_func_t>(onTextSet),reinterpret_cast<dobby_dummy_func_t*>(&textSetOriginal))==0&&textSetOriginal;
    bool b=DobbyHook(reinterpret_cast<void*>(image.base+ms::abi::at(ms::abi::uiTextPopulate)),reinterpret_cast<dobby_dummy_func_t>(onPopulate),reinterpret_cast<dobby_dummy_func_t*>(&populateOriginal))==0&&populateOriginal;
    bootLog(pkg,a||b?"uid-hide":"uid-hide-disabled",std::string("UI.Text.set_text ")+(a?"已挂钩":"挂钩失败")+"，UI.Text.OnPopulateMesh "+(b?"已挂钩":"挂钩失败")+"（按“UID:数字”识别，不改动 UID 本身）");
    if(a||b)std::thread([pkg]{unsigned said=0;for(int i=0;i<90;i++){std::this_thread::sleep_for(std::chrono::seconds(20));unsigned n=uidHidden.load();if(n!=said){said=n;bootLog(pkg,"uid-hide-count","已隐藏 UID 文本 "+std::to_string(n)+" 次");}}}).detach();}}}
 // Offsets of this build not found yet (offsets.hpp pendingRequired): nothing is hooked or called.
 if(*ms::abi::pendingRequired){bootStatus(pkg,"offsets-pending",std::string("此版本还缺偏移：")+ms::abi::pendingRequired);return;}
 ms::Engine engine(image.base,managed.base);
 auto rt=std::make_unique<ms::Runtime>(std::move(engine),pkg,priv);
 // Publish before Dobby makes Tick executable through its trampoline.
 runtime=std::move(rt);published.store(runtime.get(),std::memory_order_release);
 int result=DobbyInstrument(reinterpret_cast<void*>(image.base+ms::abi::at(ms::abi::tick)),onTick);
 if(result!=0){published.store(nullptr,std::memory_order_release);runtime.reset();throw std::runtime_error("DobbyInstrument 失败："+std::to_string(result));}bootStatus(pkg,"ready","模型运行时已就绪");
 // Replacement at the hand-off; without it the scan still replaces, just not before activation.
 {std::string bad;for(auto& c:ms::abi::handoffChecks){if(!ms::abi::found(ms::abi::at(c.rva))){bad+=std::string(ms::abi::nameOf(c.rva))+" 未解析 ";continue;}if(!codeMatches(image.base,c)){char b[24];std::snprintf(b,sizeof(b),"0x%llx ",static_cast<unsigned long long>(ms::abi::at(c.rva)));bad+=b;}}
  if(!bad.empty())bootLog(pkg,"spawn-hook-disabled","不可用："+bad);
  else if(DobbyHook(reinterpret_cast<void*>(image.base+ms::abi::at(ms::abi::receive)),reinterpret_cast<dobby_dummy_func_t>(onReceive),reinterpret_cast<dobby_dummy_func_t*>(&receiveOriginal))!=0||!receiveOriginal)bootLog(pkg,"spawn-hook-disabled","DobbyHook 失败");
  else bootLog(pkg,"spawn-hook","已安装");}
 // Mesh + bone binding: each entry is hooked only when its own code bytes match.
 for(size_t i=0;i<std::size(ms::abi::bindChecks);i++){auto& c=ms::abi::bindChecks[i];const bool lod=i==0;const char* what=lod?"lod-bind-hook":"mesh-bones-hook";
  if(!ms::abi::found(ms::abi::at(c.rva))){bootLog(pkg,std::string(what)+"-disabled","本版本尚未找到偏移");continue;}
  if(!codeMatches(image.base,c)){bootLog(pkg,std::string(what)+"-disabled","入口字节不符");continue;}
  auto replacement=lod?reinterpret_cast<dobby_dummy_func_t>(onLodBind):reinterpret_cast<dobby_dummy_func_t>(onMeshBones);auto* original=reinterpret_cast<dobby_dummy_func_t*>(lod?&lodBindOriginal:&meshBonesOriginal);
  if(DobbyHook(reinterpret_cast<void*>(image.base+ms::abi::at(c.rva)),replacement,original)!=0||!*original)bootLog(pkg,std::string(what)+"-disabled","DobbyHook 失败");else bootLog(pkg,what,"已安装");}
 }catch(const std::exception& e){bootStatus(pkg,"error",e.what());}}).detach();
 }catch(const std::exception& e){exception(env,e);} }
// The converter's 3DMigoto backend: what the INI draws (logic_import.hpp drawsJson), UTF-8 JSON in and out.
extern "C" JNIEXPORT jbyteArray JNICALL Java_com_hsrbox_tool_NativeBridge_drawsUtf8(JNIEnv* env,jclass,jbyteArray input){try{ms::require(input!=nullptr,"绘制请求为空");auto n=env->GetArrayLength(input);ms::require(n>0&&n<=16*1024*1024,"绘制请求大小无效");
 std::string request(size_t(n),'\0');env->GetByteArrayRegion(input,0,n,reinterpret_cast<jbyte*>(request.data()));if(env->ExceptionCheck())return nullptr;auto out=ms::drawsJson(request);
 auto a=env->NewByteArray(jsize(out.size()));if(!a)return nullptr;env->SetByteArrayRegion(a,0,jsize(out.size()),reinterpret_cast<const jbyte*>(out.data()));return a;}catch(const std::exception& e){exception(env,e);return nullptr;}}
// The converter's Present rehearsal (logic_import.hpp): UTF-8 JSON in, UTF-8 JSON out.
extern "C" JNIEXPORT jbyteArray JNICALL Java_com_hsrbox_tool_NativeBridge_rehearseUtf8(JNIEnv* env,jclass,jbyteArray input){try{ms::require(input!=nullptr,"预演请求为空");auto n=env->GetArrayLength(input);ms::require(n>0&&n<=16*1024*1024,"预演请求大小无效");
 std::string request(size_t(n),'\0');env->GetByteArrayRegion(input,0,n,reinterpret_cast<jbyte*>(request.data()));if(env->ExceptionCheck())return nullptr;auto out=ms::rehearseJson(request);
 auto a=env->NewByteArray(jsize(out.size()));if(!a)return nullptr;env->SetByteArrayRegion(a,0,jsize(out.size()),reinterpret_cast<const jbyte*>(out.data()));return a;}catch(const std::exception& e){exception(env,e);return nullptr;}}
extern "C" JNIEXPORT jbyteArray JNICALL Java_com_hsrbox_tool_NativeBridge_decodeDds(JNIEnv* env,jclass,jbyteArray input){try{ms::require(input!=nullptr,"DDS 数据为空");auto n=env->GetArrayLength(input);ms::require(n>=128&&n<=256*1024*1024,"DDS input size");ms::Bytes b(n);env->GetByteArrayRegion(input,0,n,reinterpret_cast<jbyte*>(b.data()));if(env->ExceptionCheck())return nullptr;auto t=ms::shrinkTexture(ms::decodeDds(b),1024);ms::Bytes out(8+t.pixels.size());ms::put32(out.data(),t.w);ms::put32(out.data()+4,t.h);std::memcpy(out.data()+8,t.pixels.data(),t.pixels.size());auto a=env->NewByteArray(out.size());if(!a)return nullptr;env->SetByteArrayRegion(a,0,out.size(),reinterpret_cast<jbyte*>(out.data()));return a;}catch(const std::exception& e){exception(env,e);return nullptr;}}
