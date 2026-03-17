// Standalone driver for replaying fuzzer corpus under coverage instrumentation.
// Avoids linking libFuzzer (libclang_rt.fuzzer), which may be built against a
// different C++ stdlib than the rest of the project.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv);

int
main(int argc, char** argv)
{
    LLVMFuzzerInitialize(&argc, &argv);

    for (int i = 1; i < argc; i++)
    {
        FILE* f = fopen(argv[i], "rb");
        if (!f)
        {
            fprintf(stderr, "cannot open %s\n", argv[i]);
            continue;
        }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> buf(len);
        fread(buf.data(), 1, len, f);
        fclose(f);
        LLVMFuzzerTestOneInput(buf.data(), buf.size());
    }
    return 0;
}
