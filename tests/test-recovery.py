"""Exercise the real generation/import/response gates without Vulkan or a game."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


layer = (ROOT / 'layer_linux/src/layer.cpp').read_text()
helper = (ROOT / 'helper/main.cpp').read_text()
begin = helper.index('    // Retire the previous incarnation')
restart = helper[begin:helper.index('    // Announced before anything slow', begin)]
begin = helper.index('        const uint32_t waitValue=shm.hdr->gpuWaitValue.load();')
request = helper[begin:helper.index('        if (!ok) Log(', begin)]
functions = '\n'.join(block(layer, marker) for marker in [
    'static bool GpuImagesRequested(', 'static void QuarantineGpuTransport(',
    'static bool EnsureGpuTimeline(', 'static bool ModelTimelineCompleted(',
    'static bool ShmProcessFrame('])
fixture = r'''
#include <vulkan/vulkan.h>
#include "common/shm_protocol.h"
#include <cassert>
#include <chrono>
#include <thread>
#include <vector>
#include <memory>
#include <map>
struct ShmMap { ShmHeader* hdr=nullptr; bool dead=false,everAnswered=false; uint32_t timeouts=0;
 double retryAfterMs=0; unsigned char* inPixels=nullptr;unsigned char* outPixels=nullptr; };
unsigned idle_calls=0,imports=0,destroys=0,drops=0;
uint64_t gpu_completed=0; bool counter_ok=true,change_during_import=false;
ShmHeader h;
struct Composition {bool model=true;void InvalidateXfer(){assert(idle_calls);++drops;}void ForgetModelFrame(){model=false;}};
struct Swapchain {std::unique_ptr<Composition> comp=std::make_unique<Composition>();uint32_t proxySeqSeen=1,answerSeqSeen=1;};
VkResult VKAPI_PTR create_sem(VkDevice,const VkSemaphoreCreateInfo*,const VkAllocationCallbacks*,VkSemaphore* out){
 *out=reinterpret_cast<VkSemaphore>(uintptr_t(13+imports));return VK_SUCCESS;}
void VKAPI_PTR destroy_sem(VkDevice,VkSemaphore,const VkAllocationCallbacks*){++destroys;}
VkResult VKAPI_PTR import_sem(VkDevice,const VkImportSemaphoreFdInfoKHR*){
 ++imports;if(change_during_import)h.gpuEpoch.fetch_add(1);return VK_SUCCESS;}
VkResult VKAPI_PTR get_counter(VkDevice,VkSemaphore,uint64_t* out){*out=gpu_completed;return counter_ok?VK_SUCCESS:VK_ERROR_DEVICE_LOST;}
PFN_vkVoidFunction VKAPI_PTR lookup(VkDevice,const char* name){
 if(!strcmp(name,"vkImportSemaphoreFdKHR"))return reinterpret_cast<PFN_vkVoidFunction>(import_sem);
 if(!strcmp(name,"vkGetSemaphoreCounterValue"))return reinterpret_cast<PFN_vkVoidFunction>(get_counter);
 return nullptr;
}
struct DeviceChain {ShmMap shm;bool gpuTransportCapable=true;VkDevice self{};
 VkSemaphore transportTimeline{};uint32_t timelinePid=0,timelineFd=0,timelineEpoch=0,quarantinedEpoch=0;uint64_t timelineValue=0;
 std::map<int,Swapchain> swapchains;PFN_vkGetDeviceProcAddr next_dpa=lookup;
 PFN_vkCreateSemaphore vkCreateSemaphore=create_sem;PFN_vkDestroySemaphore vkDestroySemaphore=destroy_sem;};
template<class... T>void Log(const char*,T...){}
VkResult DeviceIdleLocked(DeviceChain*){++idle_calls;return VK_SUCCESS;}
bool NoteVk(DeviceChain*,VkResult result,const char*){return result==VK_SUCCESS;}
int AdoptPeerFd(uint32_t,uint32_t){return dup(STDOUT_FILENO);}
bool ShmOpen(ShmMap& s){return s.hdr!=nullptr;}
bool ShmMapFrames(ShmMap&,size_t){return true;}
bool TimeEnabled(){return false;}int TimeInterval(){return 30;}
void CpuYield(){}
double now=0;int response_mode=0;
double NowMs(){
 now+=10;
 if(h.seq_req.load() && response_mode){
  h.answeredW=64;h.answeredH=64;h.seq_ok=h.seq_req.load();h.seq_resp=h.seq_req.load();
  h.gpuResponseEpoch=response_mode==2?1:h.gpuEpoch.load();
  if(response_mode==3)h.gpuEpoch.fetch_add(1);
  if(response_mode==4)h.seq_resp.fetch_add(1);
 }
 return now;
}
''' + functions + r'''
unsigned boundaries=0,evaluations=0;
template<class T>bool TimelineBoundary(T&,uint32_t,uint32_t){++boundaries;return true;}
template<class T>bool ProcessFrame(T&,ShmMap&){++evaluations;return true;}
bool helper_request(uint32_t helperEpoch){
 struct {struct {bool dmaBuf=true;}vk;}ns;ShmMap shm;shm.hdr=&h;
''' + request + r'''
 return ok;
}
void restart_helper(){ShmMap s;s.hdr=&h;
''' + restart + r'''
}
void response_test(int mode,bool accepted){
 ShmInitDefaults(&h);h.gpuEpoch=2;h.gpuTimelineReady=1;h.helperState=kHelperRunning;
 unsigned char in[64*64*4]{},out[sizeof in]{};ShmMap s;s.hdr=&h;s.inPixels=in;s.outPixels=out;
 response_mode=mode;now=0;
 assert(ShmProcessFrame(s,64,64,sizeof in,in,out,true,true,false,2)==accepted);
 assert(h.gpuRequestEpoch==2);
}
int main(){
 static_assert(offsetof(ShmHeader,gpuEpoch)==2016 && offsetof(ShmHeader,gpuTimelinePid)==1972);
 setenv("DLSSNR_GPU_IMAGES","1",1);ShmInitDefaults(&h);assert(kShmVersion==23);
 h.gpuEpoch=7;h.gpuRequestEpoch=7;h.gpuResponseEpoch=7;ShmResetSettings(&h);
 assert(h.gpuEpoch==7 && h.gpuRequestEpoch==7 && h.gpuResponseEpoch==7);
 h.seq_req=9;h.seq_resp=8;h.seq_ok=8;h.gpuTimelineReady=1;h.proxyExportSeq=1;h.answerExportSeq=1;
 restart_helper();assert(h.gpuEpoch==8 && h.seq_resp==9 && h.seq_ok==0);
 assert(!h.gpuTimelineReady && !h.proxyExportSeq && !h.answerExportSeq && !h.gpuRequestEpoch && !h.gpuResponseEpoch);
 DeviceChain d;d.shm.hdr=&h;d.swapchains.try_emplace(1);
 assert(!EnsureGpuTimeline(&d) && !imports);
 h.gpuTimelineReady=1;h.gpuTimelinePid=42;h.gpuTimelineFd=5;
 assert(EnsureGpuTimeline(&d) && imports==1 && d.timelineEpoch==8);
 QuarantineGpuTransport(&d,"test timeout");assert(!EnsureGpuTimeline(&d) && imports==1);
 h.gpuEpoch=9; // The same numeric PID/fd must still import fresh ownership.
 assert(EnsureGpuTimeline(&d) && imports==2 && drops==1 && destroys==1);
 assert(!d.swapchains[1].comp->model && !d.swapchains[1].proxySeqSeen && !d.swapchains[1].answerSeqSeen);
 gpu_completed=2;assert(ModelTimelineCompleted(&d,2));assert(!ModelTimelineCompleted(&d,3));
 counter_ok=false;assert(!ModelTimelineCompleted(&d,1));counter_ok=true;
 h.gpuTimelineFd=6;assert(!EnsureGpuTimeline(&d) && d.quarantinedEpoch==9);
 h.gpuEpoch=10;change_during_import=true;assert(!EnsureGpuTimeline(&d) && !d.transportTimeline);
 assert(destroys==3); // old import and the racing new import both retired
 h.gpuEpoch=9;h.gpuRequestEpoch=8;h.gpuWaitValue=1;h.gpuSignalValue=2;
 assert(!helper_request(9) && !boundaries && !evaluations);
 h.gpuRequestEpoch=9;assert(helper_request(9) && boundaries==2 && evaluations==1);
 h.gpuEpoch=10;assert(!helper_request(9) && boundaries==2 && evaluations==1);
 response_test(1,true);response_test(2,false);response_test(3,false);response_test(4,false);
 response_test(0,false); // bounded silent helper wait, without any GPU submission
 puts("PASS: generation restart, quarantine, import retirement/race, completed timeline and exact response gates");
}
'''
with tempfile.TemporaryDirectory(prefix='dlss-recovery-') as tmp:
    cpp, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
    cpp.write_text(fixture)
    subprocess.run(['g++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-Wno-missing-field-initializers', '-I' + str(ROOT),
                    '-I' + str(ROOT / 'standalone_runner/third_party'), str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
