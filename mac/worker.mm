#include "pipeline.h"
#include "paramfile.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <unistd.h>

int main(int argc, char** argv) {
    // Keep all renderer diagnostics off the binary response stream.
    FILE* output = fdopen(dup(STDOUT_FILENO), "wb");
    dup2(STDERR_FILENO, STDOUT_FILENO);
    if (argc != 2 || !output) return 2;
    try {
        metal::AutoreleaseScope pool;
        std::string error;
        if (!metal::init(nullptr, &error)) throw std::runtime_error(error);
        Pipeline pipeline;
        if (!pipeline.init()) throw std::runtime_error("Shader initialization failed");
        const std::array<uint32_t, 4> ready = {0x31544450, 0, 0, 0};
        fwrite(ready.data(), sizeof(ready), 1, output);
        fflush(output);
        uint32_t frame = 0;
        std::string previousSettings;
        for (;;) {
            metal::AutoreleaseScope framePool;
            std::array<uint32_t, 4> header;
            if (fread(header.data(), sizeof(header), 1, stdin) != 1) break;
            const uint32_t w = header[1], h = header[2];
            if (header[0] != 0x31544450 || !w || !h || w > 4096 || h > 4096 ||
                uint64_t(w) * h > 8388608) throw std::runtime_error("Invalid frame header");
            std::vector<unsigned char> rgba(size_t(w) * h * 4);
            if (fread(rgba.data(), rgba.size(), 1, stdin) != 1) break;
            TuningParams tuning;
            RenderConfig config;
            StyleParams style;
            std::ifstream settingsFile(argv[1]);
            std::string settings((std::istreambuf_iterator<char>(settingsFile)), {});
            if (settings != previousSettings) {
                pipeline.resetTemporal();
                previousSettings = settings;
            }
            if (!paramfile::load(argv[1], &tuning, &config, &error, &style))
                throw std::runtime_error(error);
            tuning.frame = float(frame++);
            pipeline.setStyle(style);
            if (!pipeline.setSource(rgba.data(), int(w), int(h)))
                throw std::runtime_error("Invalid source frame");
            pipeline.render(tuning, config, header[3] != 0);
            rgba = pipeline.readCanvas();
            const std::array<uint32_t, 4> response = {0x31544450, w, h, uint32_t(rgba.size())};
            if (fwrite(response.data(), sizeof(response), 1, output) != 1 ||
                fwrite(rgba.data(), rgba.size(), 1, output) != 1 || fflush(output)) break;
        }
        pipeline.shutdown();
        fclose(output);
        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "Paintify Metal: %s\n", e.what());
        fclose(output);
        return 1;
    }
}
