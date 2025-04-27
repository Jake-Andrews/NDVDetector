#include "PHashCalculator.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <spdlog/spdlog.h>

// SIMD headers
#if defined(__x86_64__) || defined(_M_X64)
#    include <immintrin.h> // AVX/SSE
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#    include <arm_neon.h>
#endif

// Static member initialization
float PHashCalculator::DCT_MATRIX_32[32][32] = { { 0 } };
bool PHashCalculator::dctMatrixInitialized = false;

void PHashCalculator::ensureDCTMatrixInitialized()
{
    if (!dctMatrixInitialized) {
        initializeDCTMatrix(DCT_MATRIX_32, 32);
        dctMatrixInitialized = true;
    }
}

std::optional<uint64_t> PHashCalculator::calculatePHash(float const* imageData, int width, int height)
{
    // Ensure the DCT matrix is initialized
    ensureDCTMatrixInitialized();

    if (!imageData || width != 32 || height != 32) {
        spdlog::error("[pHash] Invalid input data or dimensions for pHash calculation.");
        return std::nullopt;
    }

    try {
        // Intermediate storage for DCT computation
        float dctCoeffs[32 * 32] = { 0 };

        // Apply DCT transform to the input image
        applyDCT(imageData, dctCoeffs, 32);

        // Extract the 8x8 low frequency components (excluding DC component)
        // starting from position (1,1) as in the original OpenCV implementation
        std::vector<float> coefficients;
        coefficients.reserve(64);

        for (int y = 1; y < 9; y++) {
            for (int x = 1; x < 9; x++) {
                coefficients.push_back(dctCoeffs[y * 32 + x]);
            }
        }

        // Calculate median of DCT coefficients
        std::vector<float> sortedCoeffs = coefficients;
        std::sort(sortedCoeffs.begin(), sortedCoeffs.end());
        float median = sortedCoeffs[sortedCoeffs.size() / 2];

        // Generate 64-bit hash by comparing each coefficient to the median
        uint64_t hash = 0;
        for (int i = 0; i < 64; i++) {
            // Set bit if coefficient is greater than median
            if (coefficients[i] > median) {
                hash |= (1ULL << (63 - i));
            }
        }

        spdlog::debug("[pHash] Calculated pHash: {:#018x}", hash);
        return hash;
    } catch (std::exception const& e) {
        spdlog::error("[pHash] Standard exception during pHash calculation: {}", e.what());
        return std::nullopt;
    } catch (...) {
        spdlog::error("[pHash] Unknown exception during pHash calculation.");
        return std::nullopt;
    }
}

std::optional<uint64_t> PHashCalculator::calculatePHash(uint8_t const* imageData, int width, int height)
{
    if (!imageData || width != 32 || height != 32) {
        spdlog::error("[pHash] Invalid input data or dimensions for pHash calculation.");
        return std::nullopt;
    }

    // Convert uint8_t to float (0.0 - 255.0 range)
    float floatData[32 * 32];
    for (int i = 0; i < width * height; i++) {
        floatData[i] = static_cast<float>(imageData[i]);
    }

    return calculatePHash(floatData, width, height);
}

void PHashCalculator::applyDCT(float const* input, float* output, int size)
{
    // Temporary storage for intermediate results
    float* temp = new float[size * size];

    // DCT = DCT_MATRIX * input * DCT_MATRIX^T
    // First multiply: temp = DCT_MATRIX * input
    matrixMultiplySIMD(&DCT_MATRIX_32[0][0], input, temp, size);

    // Transpose DCT matrix for second multiplication
    float* transposed = new float[size * size];
    transposeMatrix(&DCT_MATRIX_32[0][0], transposed, size);

    // Second multiply: output = temp * DCT_MATRIX^T
    matrixMultiplySIMD(temp, transposed, output, size);

    // Free temporary storage
    delete[] temp;
    delete[] transposed;
}

void PHashCalculator::transposeMatrix(float const* input, float* output, int size)
{
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            output[j * size + i] = input[i * size + j];
        }
    }
}

void PHashCalculator::matrixMultiply(float const* A, float const* B, float* C, int size)
{
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            float sum = 0.0f;
            for (int k = 0; k < size; k++) {
                sum += A[i * size + k] * B[k * size + j];
            }
            C[i * size + j] = sum;
        }
    }
}

void PHashCalculator::matrixMultiplySIMD(float const* A, float const* B, float* C, int size)
{
#if defined(__AVX__) && (defined(__x86_64__) || defined(_M_X64))
    // AVX implementation (8 floats at once)
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            __m256 sum = _mm256_setzero_ps();

            // Process 8 elements at a time
            for (int k = 0; k < size; k += 8) {
                if (k + 8 <= size) {
                    __m256 a = _mm256_loadu_ps(&A[i * size + k]);
                    __m256 b0 = _mm256_set_ps(
                        B[(k + 7) * size + j], B[(k + 6) * size + j], B[(k + 5) * size + j], B[(k + 4) * size + j],
                        B[(k + 3) * size + j], B[(k + 2) * size + j], B[(k + 1) * size + j], B[k * size + j]);
                    sum = _mm256_add_ps(sum, _mm256_mul_ps(a, b0));
                } else {
                    // Handle remaining elements
                    for (int m = k; m < size; m++) {
                        C[i * size + j] += A[i * size + m] * B[m * size + j];
                    }
                }
            }

            // Horizontal sum of the 8 results
            float result[8];
            _mm256_storeu_ps(result, sum);
            C[i * size + j] = result[0] + result[1] + result[2] + result[3] + result[4] + result[5] + result[6] + result[7];
        }
    }
#elif defined(__SSE__) && (defined(__x86_64__) || defined(_M_X64))
    // SSE implementation (4 floats at once)
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            __m128 sum = _mm_setzero_ps();

            // Process 4 elements at a time
            for (int k = 0; k < size; k += 4) {
                if (k + 4 <= size) {
                    __m128 a = _mm_loadu_ps(&A[i * size + k]);
                    __m128 b0 = _mm_set_ps(
                        B[(k + 3) * size + j], B[(k + 2) * size + j], B[(k + 1) * size + j], B[k * size + j]);
                    sum = _mm_add_ps(sum, _mm_mul_ps(a, b0));
                } else {
                    // Handle remaining elements
                    for (int m = k; m < size; m++) {
                        C[i * size + j] += A[i * size + m] * B[m * size + j];
                    }
                }
            }

            // Horizontal sum of the 4 results
            float result[4];
            _mm_storeu_ps(result, sum);
            C[i * size + j] = result[0] + result[1] + result[2] + result[3];
        }
    }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    // ARM NEON implementation (4 floats at once)
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            float32x4_t sum_vec = vdupq_n_f32(0);

            // Process 4 elements at a time
            for (int k = 0; k < size; k += 4) {
                if (k + 4 <= size) {
                    float32x4_t a_vec = vld1q_f32(&A[i * size + k]);
                    float32x4_t b_vec = {
                        B[k * size + j], B[(k + 1) * size + j], B[(k + 2) * size + j], B[(k + 3) * size + j]
                    };
                    sum_vec = vmlaq_f32(sum_vec, a_vec, b_vec);
                } else {
                    // Handle remaining elements
                    for (int m = k; m < size; m++) {
                        C[i * size + j] += A[i * size + m] * B[m * size + j];
                    }
                }
            }

            // Horizontal sum of the 4 results
            float32x2_t sum_lo = vget_low_f32(sum_vec);
            float32x2_t sum_hi = vget_high_f32(sum_vec);
            sum_lo = vadd_f32(sum_lo, sum_hi);
            sum_lo = vpadd_f32(sum_lo, sum_lo);
            C[i * size + j] = vget_lane_f32(sum_lo, 0);
        }
    }
#else
    // Fallback to standard matrix multiplication
    matrixMultiply(A, B, C, size);
#endif
}

void PHashCalculator::initializeDCTMatrix(float matrix[32][32], int N)
{
    // Generate DCT basis functions
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            if (i == 0) {
                matrix[i][j] = 1.0f / std::sqrt(static_cast<float>(N));
            } else {
                matrix[i][j] = std::sqrt(2.0f / N) * std::cos((2 * j + 1) * i * M_PI / (2 * N));
            }
        }
    }
}
