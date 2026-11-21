#include "VulkanInterpolator.h"
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
struct Image { uint32_t width{}, height{}; std::vector<uint32_t> pixels; };
std::string token(std::istream& input) {
    std::string value;
    while (input >> value) {
        if (value.starts_with('#')) { input.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); continue; }
        return value;
    }
    throw std::runtime_error("Truncated PPM header");
}
uint32_t number(const std::string& text) {
    uint32_t result = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size()) throw std::runtime_error("Invalid PPM number");
    return result;
}
Image read(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error(std::string("Cannot open ") + path);
    if (token(input) != "P6") throw std::runtime_error("Only binary P6 PPM is accepted");
    Image image;
    image.width = number(token(input)); image.height = number(token(input));
    if (!image.width || !image.height || image.width > 4096 || image.height > 4096 || number(token(input)) != 255)
        throw std::runtime_error("PPM must be 8-bit RGB, dimensions 1..4096");
    int separator = input.get();
    if (separator != ' ' && separator != '\n' && separator != '\r' && separator != '\t')
        throw std::runtime_error("Missing PPM data separator");
    if (separator == '\r' && input.peek() == '\n') input.get();
    image.pixels.resize(size_t(image.width) * image.height);
    std::vector<unsigned char> rgb(image.pixels.size() * 3);
    if (!input.read(reinterpret_cast<char*>(rgb.data()), rgb.size())) throw std::runtime_error("Truncated PPM pixels");
    for (size_t i = 0; i < image.pixels.size(); ++i)
        image.pixels[i] = uint32_t(rgb[3*i]) | uint32_t(rgb[3*i+1]) << 8 | uint32_t(rgb[3*i+2]) << 16 | 0xff000000u;
    return image;
}
void write(const char* path, const Image& image) {
    // Refuse to clobber a source image or any existing output.
    if (std::filesystem::exists(path)) throw std::runtime_error("Output exists; choose a new path");
    std::ofstream output(path, std::ios::binary);
    output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (uint32_t pixel : image.pixels) {
        const char rgb[]{char(pixel & 255), char((pixel >> 8) & 255), char((pixel >> 16) & 255)};
        output.write(rgb, 3);
    }
    if (!output) throw std::runtime_error("Failed writing PPM output");
}
}
int main(int argc, char** argv) {
    try {
        if (argc < 4 || argc > 6) {
            std::cerr << "Usage: veyra_linux_interpolate A.ppm B.ppm output.ppm [phase=0.5] [shader-directory]\n"
                      << "Diagnostic only: SDR RGB, synchronous GPU readback, no live playback.\n";
            return 2;
        }
        auto a = read(argv[1]); auto b = read(argv[2]);
        if (a.width != b.width || a.height != b.height) throw std::runtime_error("Frame dimensions differ");
        float phase = .5f;
        if (argc >= 5) {
            const std::string value(argv[4]);
            auto [end, error] = std::from_chars(value.data(), value.data()+value.size(), phase);
            if (error != std::errc{} || end != value.data()+value.size()) throw std::runtime_error("Invalid phase");
        }
        veyra::linuxgpu::VulkanInterpolator gpu(a.width, a.height, argc == 6 ? argv[5] : VEYRA_SHADER_DIR);
        const auto start = std::chrono::steady_clock::now();
        Image result{a.width, a.height, gpu.interpolate(a.pixels, b.pixels, phase)};
        const auto end = std::chrono::steady_clock::now();
        write(argv[3], result);
        std::cout << "Vulkan device: " << gpu.deviceName() << "\n"
                  << "Generated phase " << phase << ", " << a.width << 'x' << a.height << "\n"
                  << "Diagnostic submit/wait/readback: "
                  << std::chrono::duration<double, std::milli>(end-start).count() << " ms\n"
                  << "This is not a display FPS or end-to-end latency measurement.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
