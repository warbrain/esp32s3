#include "engine/framework/core/backend.h"

#include <ggml-alloc.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>

int main()
{
    try
    {
        engine::core::BackendConfig config;
        config.type = engine::core::BackendType::Metal;
        const std::unique_ptr<ggml_backend, decltype(&ggml_backend_free)> backend(
            engine::core::init_backend(config), &ggml_backend_free);
        if (!backend || engine::core::backend_type(backend.get()) != engine::core::BackendType::Metal)
            throw std::runtime_error("Metal backend was not selected");

        constexpr std::size_t terms = 32;
        constexpr std::size_t rows = 8;
        constexpr std::size_t columns = 4;
        constexpr std::size_t nodes = 16;
        const std::unique_ptr<ggml_context, decltype(&ggml_free)> context(
            ggml_init({3 * ggml_tensor_overhead() + ggml_graph_overhead_custom(nodes, false), nullptr, true}),
            &ggml_free);
        if (!context)
            throw std::runtime_error("Could not allocate graph metadata");

        auto * lhs = ggml_new_tensor_2d(context.get(), GGML_TYPE_F32, terms, rows);
        auto * rhs = ggml_new_tensor_2d(context.get(), GGML_TYPE_F32, terms, columns);
        auto * result = ggml_mul_mat(context.get(), lhs, rhs);
        auto * graph = ggml_new_graph_custom(context.get(), nodes, false);
        ggml_build_forward_expand(graph, result);
        if (!ggml_backend_supports_op(backend.get(), result))
            throw std::runtime_error("Metal does not support the F32 matrix operation");

        const std::unique_ptr<ggml_backend_buffer, decltype(&ggml_backend_buffer_free)> buffer(
            ggml_backend_alloc_ctx_tensors(context.get(), backend.get()), &ggml_backend_buffer_free);
        if (!buffer)
            throw std::runtime_error("Could not allocate Metal tensors");

        std::array<float, rows * terms> left{};
        std::array<float, columns * terms> right{};
        for (std::size_t i = 0; i < left.size(); ++i)
            left[i] = (static_cast<int>(i % 11) - 5) * 0.25f;
        for (std::size_t i = 0; i < right.size(); ++i)
            right[i] = (static_cast<int>(i % 7) - 3) * 0.125f;
        ggml_backend_tensor_set(lhs, left.data(), 0, ggml_nbytes(lhs));
        ggml_backend_tensor_set(rhs, right.data(), 0, ggml_nbytes(rhs));
        if (ggml_backend_graph_compute(backend.get(), graph) != GGML_STATUS_SUCCESS)
            throw std::runtime_error("Metal graph execution failed");

        std::array<float, rows * columns> actual{};
        ggml_backend_tensor_get(result, actual.data(), 0, ggml_nbytes(result));
        for (std::size_t column = 0; column < columns; ++column)
        {
            for (std::size_t row = 0; row < rows; ++row)
            {
                float expected = 0.0f;
                for (std::size_t term = 0; term < terms; ++term)
                    expected += left[row * terms + term] * right[column * terms + term];
                const float value = actual[column * rows + row];
                if (!std::isfinite(value) || std::abs(value - expected) > 1e-4f)
                    throw std::runtime_error("Metal matrix result differs from the scalar reference");
            }
        }
        std::printf("Metal F32 matrix smoke passed on %s\n", ggml_backend_name(backend.get()));
        return 0;
    }
    catch (const std::exception & error)
    {
        std::fprintf(stderr, "Metal smoke failed: %s\n", error.what());
        return 1;
    }
}
