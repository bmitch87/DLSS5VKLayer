"""Run actual helper export publication through resize allocation/FD failures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'helper/main.cpp').read_text()


def block(marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


fixture = r'''
#include "common/shm_protocol.h"
#include <cassert>
#include <cstdio>
using VkFormat=uint32_t;using VkDeviceSize=uint64_t;
struct GpuImage {uint64_t image=0;uint32_t width=0,height=0;VkFormat format=0;};
struct VkCtx {
 GpuImage proxyIn,answerOut;int proxyExportFd=-1,answerExportFd=-1;
 uint32_t proxyW=0,proxyH=0,proxyGen=0,proxySeq=0,answerGen=0,answerSeq=0,linuxPid=42;
 VkDeviceSize proxyAllocation=0,answerAllocation=0;uint32_t proxyMemoryType=0,answerMemoryType=0;
};
int failure=0;
bool CreateExportable(VkCtx&,GpuImage& image,int& fd,uint32_t w,uint32_t h,VkFormat format,
                      const char*,VkDeviceSize& allocation,uint32_t& type){
 if(image.image && image.width==w && image.height==h && image.format==format && fd>=0)return true;
 fd=-1;
 if(failure==1)return false; // Closing the old exported FD failed; old image object remains.
 image={};
 if(failure==2)return false; // New image/allocation/export failed after old image destruction.
 image={17,w,h,format};fd=23;allocation=uint64_t(w)*h*4;type=3;
 return true; // Deliberately reuse numeric image/fd handles to test sequence ownership.
}
''' + block('static bool EnsureProxyOut(') + '\n' + block('static bool EnsureAnswerOut(') + r'''
int main(){
 for(bool proxy:{false,true}){
  VkCtx c;ShmHeader h;ShmInitDefaults(&h);
  auto ensure=[&](unsigned w,unsigned height){return proxy?EnsureProxyOut(c,&h,w,height,1):EnsureAnswerOut(c,&h,w,height,1);};
  auto sequence=[&]{return proxy?h.proxyExportSeq.load():h.answerExportSeq.load();};
  assert(ensure(576,324) && sequence()==1);
  assert(ensure(576,324) && sequence()==1); // Stable raster retains its existing export.
  failure=1;assert(!ensure(640,360) && sequence()==0);
  assert((proxy?c.proxyIn.image:c.answerOut.image)!=0);
  if(proxy)assert(!c.proxyW && !c.proxyH);
  failure=0;assert(ensure(640,360) && sequence()==2);
  failure=2;assert(!ensure(576,324) && sequence()==0);
  assert((proxy?c.proxyIn.image:c.answerOut.image)==0);
  failure=0;assert(ensure(576,324) && sequence()==3);
  assert(ensure(576,324) && sequence()==3);
 }
 puts("PASS: proxy/answer resize failures withdraw stale FDs; retries publish fresh sequences despite handle reuse");
}
'''
with tempfile.TemporaryDirectory(prefix='dlss-resize-') as temp:
    cpp, exe = Path(temp) / 'test.cpp', Path(temp) / 'test'
    cpp.write_text(fixture)
    subprocess.run(['g++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT),
                    str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
