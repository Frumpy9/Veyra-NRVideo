#include "VulkanInterpolator.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <iostream>
#include <stdexcept>

uint32_t number(const char* arg) {
    std::string text(arg);
    uint32_t value=0;
    auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
    if(error!=std::errc{} || end!=text.data()+text.size()) throw std::invalid_argument("Invalid integer");
    return value;
}
int main(int argc, char** argv) {
    try {
        if(argc!=5) {
            std::cerr << "Usage: veyra_linux_benchmark width height iterations search-radius\n";
            return 2;
        }
        const uint32_t w=number(argv[1]),h=number(argv[2]),count=number(argv[3]),radius=number(argv[4]);
        if(count<1 || count>100 || radius>16) throw std::invalid_argument("Iterations 1..100; radius 0..16");
        veyra::linuxgpu::VulkanInterpolator gpu(w,h,VEYRA_SHADER_DIR);
        std::vector<uint32_t> a(size_t(w)*h),b(a.size());
        auto pixel=[](uint32_t x,uint32_t y) {
            uint32_t n=x*0x9e3779b9u ^ y*0x85ebca6bu;
            n^=n>>16; n*=0x7feb352du; n^=n>>15;
            return 0xff000000u|(n&0x00ffffffu);
        };
        for(uint32_t y=0;y<h;++y) for(uint32_t x=0;x<w;++x) {
            a[size_t(y)*w+x]=pixel(x,y);
            b[size_t(y)*w+x]=pixel(x>=4?x-4:0,y);
        }
        gpu.interpolate(a,b,.5f,radius); // Pipeline/driver warmup excluded.
        std::vector<double> ms;
        uint32_t checksum=0;
        for(uint32_t i=0;i<count;++i) {
            const auto start=std::chrono::steady_clock::now();
            auto result=gpu.interpolate(a,b,.5f,radius);
            ms.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
            checksum^=result[result.size()/2];
        }
        std::sort(ms.begin(),ms.end());
        std::cout << "Device: " << gpu.deviceName() << '\n'
                  << "Extent: " << w << 'x' << h << ", search radius: " << radius
                  << ", samples after warmup: " << count << '\n'
                  << "Diagnostic total ms (upload + motion + synthesis + fence + readback):\n"
                  << "min=" << ms.front() << " median=" << ms[ms.size()/2]
                  << " p95=" << ms[(ms.size()*95+99)/100-1] << " max=" << ms.back() << '\n'
                  << "No decode, color conversion, presentation, audio, or controller timing included.\n"
                  << "Not a physical FPS or end-to-end latency measurement. Checksum: " << checksum << '\n';
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
