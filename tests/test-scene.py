from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
src=(root/'helper/main.cpp').read_text()
start=src.index('static bool DetectSceneCut(')
end=src.index('// Preserve the host transport',start)
function=src[start:end]
text=r"""
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include "scene_sample_grid.h"
struct NeuralState {std::vector<uint8_t> prevLuma;uint32_t lumaW=0,lumaH=0,sceneCutStreak=0;};
void Log(const char*,...){}
"""+function+r"""
int main(){
for(auto size:std::vector<std::pair<unsigned,unsigned>>{{1,1},{63,35},{64,36},{65,37},{640,360},{1152,324},{1920,540},{3841,1081}}){
 const auto w=size.first,h=size.second,gw=dlssnr::SceneGridWidth(w),gh=dlssnr::SceneGridHeight(h);
 NeuralState host,gpu;
 std::vector<uint8_t> full(size_t(w)*h*4),samples(size_t(gw)*gh*4);
 for(unsigned frame=0;frame<12;++frame){
  for(size_t i=0;i<full.size();++i)full[i]=frame<4?(i*17+frame)%256:(frame%2?255:0);
  for(unsigned y=0;y<gh;++y)for(unsigned x=0;x<gw;++x){
   const auto px=dlssnr::SceneSampleCoordinate(x,w,gw),py=dlssnr::SceneSampleCoordinate(y,h,gh);
   assert(px<w && py<h);
   memcpy(&samples[(size_t(y)*gw+x)*4],&full[(size_t(py)*w+px)*4],4);
  }
  assert(DetectSceneCut(host,full.data(),w,h,1)==DetectSceneCut(gpu,samples.data(),gw,gh,1));
  assert(host.prevLuma==gpu.prevLuma && host.sceneCutStreak==gpu.sceneCutStreak);
 }
}
}
"""
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(text)
 subprocess.run(['g++','-std=c++17','-O2','-I'+str(root/'common'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
print('Exact scene sampling and original detector equivalence passed')
