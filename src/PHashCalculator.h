#pragma once
#include <cstdint>
#include <optional>

class PHashCalculator {
public:
    // Calculates pHash from a 32x32 single-channel float image data (0.0 - 255.0 range)
    static std::optional<uint64_t> calculatePHash(const float* imageData, int width, int height);
    
    static std::optional<uint64_t> calculatePHash(const uint8_t* imageData, int width, int height);
    
    static float DCT_MATRIX_32[32][32];
    
private:
    static void applyDCT(const float* input, float* output, int size);
    static void transposeMatrix(const float* input, float* output, int size);
    static void matrixMultiply(const float* A, const float* B, float* C, int size);
    static void initializeDCTMatrix(float matrix[32][32], int N);
    
    // SIMD-optimized matrix multiplication
    static void matrixMultiplySIMD(const float* A, const float* B, float* C, int size);
    
    // Static initialization flag
    static bool dctMatrixInitialized;
    
    // Make sure DCT matrix is initialized
    static void ensureDCTMatrixInitialized();
};
