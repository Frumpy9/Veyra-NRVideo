#include "VulkanInterpolator.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using veyra::linuxgpu::VulkanInterpolator;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F action, const char* message) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
}
uint32_t texture(int x, int y) {
    uint32_t v = uint32_t(x + 1000) * 0x9e3779b9u ^ uint32_t(y + 1000) * 0x85ebca6bu;
    v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15;
    return (v & 0x00ffffffu) | 0xff000000u;
}
std::vector<uint32_t> translated(uint32_t w, uint32_t h, int dx, int dy) {
    std::vector<uint32_t> frame(size_t(w)*h);
    for (uint32_t y=0; y<h; ++y) for (uint32_t x=0; x<w; ++x)
        frame[size_t(y)*w+x] = texture(int(x)-dx, int(y)-dy);
    return frame;
}
double error(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, uint32_t w, uint32_t h) {
    double total = 0; size_t count = 0;
    for (uint32_t y=16; y<h-16; ++y) for (uint32_t x=16; x<w-16; ++x)
        for (int c=0; c<3; ++c) {
            total += std::abs(int((a[size_t(y)*w+x]>>(8*c))&255)-int((b[size_t(y)*w+x]>>(8*c))&255));
            ++count;
        }
    return total / double(count);
}
}
int main() {
    try {
        constexpr uint32_t w=96, h=64;
        VulkanInterpolator gpu(w, h, VEYRA_SHADER_DIR);
        std::cout << "Device: " << gpu.deviceName() << '\n';
        auto a=translated(w,h,0,0), b=translated(w,h,4,0);
        require(gpu.interpolate(a,b,0)==a, "phase zero changed source A");
        require(gpu.interpolate(a,b,1)==b, "phase one changed source B");
        require(gpu.interpolate(a,a)==a, "static frame changed");
        for (auto [dx,dy] : {std::pair{4,0}, {0,4}, {-4,0}, {4,-4}}) {
            b=translated(w,h,dx,dy);
            for (float t : {.25f,.5f,.75f}) {
                auto generated=gpu.interpolate(a,b,t);
                auto expected=translated(w,h,int(float(dx)*t),int(float(dy)*t));
                double mae=error(generated,expected,w,h);
                std::cout << "Translation " << dx << ',' << dy << " phase " << t << " MAE " << mae << '\n';
                require(mae < 1.0, "generated motion did not match intermediate ground truth");
                require(error(generated,a,w,h)>10, "generated frame repeats A");
                require(error(generated,b,w,h)>10, "generated frame repeats B");
            }
        }
        std::vector<uint32_t> black(w*h,0xff000000u), white(w*h,0xffffffffu);
        require(gpu.interpolate(black,white,.25f)==black, "cut blended before midpoint");
        require(gpu.interpolate(black,white,.75f)==white, "cut blended after midpoint");
        require(gpu.interpolate(white,black,.75f)==black, "cut state leaked from previous pair");
        rejects([&]{ gpu.interpolate(a,b,-.1f); }, "negative phase accepted");
        rejects([&]{ gpu.interpolate(a,b,1.1f); }, "phase above one accepted");
        rejects([&]{ gpu.interpolate(a,b,std::numeric_limits<float>::quiet_NaN()); }, "NaN accepted");
        rejects([&]{ gpu.interpolate(a,b,.5f,17); }, "unbounded search accepted");
        rejects([&]{ gpu.interpolate(std::span(a).first(3),b); }, "bad input extent accepted");
        rejects([&]{ VulkanInterpolator invalid(0,1,VEYRA_SHADER_DIR); }, "zero dimension accepted");
        VulkanInterpolator odd(13,9,VEYRA_SHADER_DIR);
        auto small=translated(13,9,0,0);
        require(odd.interpolate(small,small)==small, "partial workgroups changed pixels");
        VulkanInterpolator tiny(1,1,VEYRA_SHADER_DIR);
        std::vector<uint32_t> single{0xffa07820u};
        require(tiny.interpolate(single,single)==single, "single pixel changed");
        std::cout << "PASS: endpoints, static, 12 motion/phase ground truths, cuts, input bounds, odd/tiny extents\n";
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
