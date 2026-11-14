#include "snapshot_gpu.hpp"
#include "transformacion_reversible.hpp"
#include "mundo_x/quantum_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

namespace {

using Clock = std::chrono::steady_clock;
using Transform = xgpu::ReversibleBitTransform;
using Gate = Transform::Gate;
using Op = Transform::Operation;
using O = xgpu::StateOp;
constexpr xgpu::Index domain = xgpu::domain_limit;
constexpr std::size_t output_words = 4'194'304;
constexpr std::size_t width = 1024;

void need(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

template<class Run>
double timed(Run run) {
    const auto start = Clock::now();
    run();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

std::string periodic_checkpoint() {
    xgpu::Execution state(1, {}, xgpu::ExecutionProfile::C12);
    state.enqueue({{{O::LocalIndex, 0}, {O::Constant, 1, 0, 0, 0, 63},
                    {O::BitXor, 2, 0, 1}, {O::Constant, 3, 0, 0, 0, 3},
                    {O::Add, 4, 2, 3}, {O::Multiply, 5, 4, 4}, {O::Output, 5}, {O::Halt}}},
                  domain, 64);
    need(state.run(), "El despacho C12 no terminó");
    return state.snapshot();
}

void old_copy(xgpu::ValueReader& reader, std::string& output) {
    for (std::size_t i = 0; i < output.size() / 4; ++i) {
        const auto value = reader.at(i);
        for (unsigned byte = 0; byte < 4; ++byte)
            output[i * 4 + byte] = static_cast<char>((value >> (8U * byte)) & 255);
    }
}

void graphics_work() {
    const auto checkpoint = periodic_checkpoint();
    std::string original(output_words * 4, '\0'), optimized(output_words * 4, '\0');
    std::vector<double> legacy_ms, new_ms;
    std::size_t evaluated = 0;
    const auto reference = [&] {
        const auto state = xgpu::Execution::restore(checkpoint);
        xgpu::ValueReader reader(state.dispatch(0).output);
        old_copy(reader, original);
    };
    const auto candidate = [&] {
        const auto state = xgpu::Execution::restore(checkpoint);
        xgpu::ValueReader reader(state.dispatch(0).output);
        evaluated = reader.copy_little_endian(0, output_words, optimized.data());
    };

    reference();
    candidate();
    for (unsigned i = 0; i < 5; ++i) {
        if (i % 2 == 0) {
            legacy_ms.push_back(timed(reference));
            new_ms.push_back(timed(candidate));
        } else {
            new_ms.push_back(timed(candidate));
            legacy_ms.push_back(timed(reference));
        }
        need(original == optimized, "Las rutas de exportación difieren");
    }
    for (std::size_t i = 0; i < output_words; ++i) {
        const auto value = (((i % 64) ^ 63) + 3);
        xgpu::Word observed = 0;
        for (unsigned byte = 0; byte < 4; ++byte)
            observed |= xgpu::Word(static_cast<unsigned char>(optimized[i * 4 + byte])) << (8U * byte);
        need(observed == value * value, "La salida no coincide con la fórmula independiente");
    }
    xgpu::Execution const state = xgpu::Execution::restore(checkpoint);
    const auto sum = xgpu::ValueReader(state.dispatch(0).output).sum_unsigned(0, domain);
    need(sum.value == (domain / 64) * (66 * 67 * 133ULL / 6 - 5) &&
         sum.evaluated_words == 64, "Suma del dominio periódico incorrecta");
    xgpu::ValueReader affine(xgpu::field_root(xgpu::Field(domain, xgpu::Mode({0, 1}))));
    const auto affine_sum = affine.sum_unsigned(0, domain);
    need(affine_sum.evaluated_words == 0 && affine_sum.value == 9'223'372'034'707'292'160ULL,
         "Suma de cuatro mil millones de índices incorrecta");
    std::cout << "grafica_palabras_logicas=" << domain << '\n'
              << "grafica_estado_serializado_bytes=" << checkpoint.size() << '\n'
              << "grafica_suma_u64=" << sum.value << '\n'
              << "grafica_palabras_evaluadas_para_la_suma=" << sum.evaluated_words << '\n'
              << "grafica_suma_afin_u64=" << affine_sum.value << '\n'
              << "grafica_palabras_evaluadas_para_suma_afin=" << affine_sum.evaluated_words << '\n'
              << "grafica_palabras_materializadas_exactas=" << output_words << '\n'
              << "grafica_palabras_evaluadas_en_exportacion=" << evaluated << '\n'
              << "grafica_mediana_c12_ms=" << median(legacy_ms) << '\n'
              << "grafica_mediana_c12_1_ms=" << median(new_ms) << '\n';
}

bool bit(const Transform::Bits& bits, std::size_t at) {
    return (bits[at / 64] >> (at % 64)) & 1U;
}

void reference_bits(Transform::Bits& bits, const std::vector<Gate>& gates) {
    for (const auto& gate : gates) {
        if (gate.operation == Op::X || bit(bits, gate.control))
            bits[gate.target / 64] ^= std::uint64_t{1} << (gate.target % 64);
    }
}

std::vector<Gate> xor_chain() {
    std::vector<Gate> circuit;
    for (std::size_t round = 0; round < 33; ++round) {
        for (std::size_t i = 0; i < width / 2; ++i)
            circuit.push_back({Op::Cnot, width / 2 + i, i});
    }
    return circuit;
}

void verify_against_xi() {
    constexpr std::size_t wires = 129;
    const std::vector<Gate> gates{{Op::X, 2}, {Op::Cnot, 64, 2}, {Op::Cnot, 128, 64},
                                  {Op::Cnot, 0, 128}, {Op::X, 128}, {Op::Cnot, 63, 0},
                                  {Op::Cnot, 64, 63}, {Op::Cnot, 128, 64}};
    const Transform compiled(wires, gates);
    std::mt19937_64 rng(43897659403ULL);
    for (unsigned sample = 0; sample < 16; ++sample) {
        Transform::Bits input{rng(), rng(), rng() & 1U};
        const auto output = compiled.apply(input);
        std::ostringstream xi;
        xi << "begin\n";
        for (std::size_t i = 0; i < wires; ++i) if (bit(input, i)) xi << "x " << i << '\n';
        xi << "end\nbegin\n";
        for (const auto& gate : gates) {
            if (gate.operation == Op::X) xi << "x " << gate.target << '\n';
            else xi << "cx " << gate.control << ' ' << gate.target << '\n';
        }
        xi << "end\n";
        for (std::size_t i = 0; i < wires; ++i)
            xi << "assert probz " << i << ' ' << bit(output, i) << '\n';
        mundo_x::QuantumRuntime runtime;
        runtime.execute(xi.str());
    }
    std::cout << "xi_bits_comparados_con_mundo_x=" << 16 * wires << '\n';
}

void reversible_work() {
    using Bits = Transform::Bits;
    const auto gates = xor_chain();
    std::vector<Bits> inputs(2048, Bits(width / 64));
    std::mt19937_64 rng(0xe7e2fed9ULL);
    for (auto& input : inputs) for (auto& word : input) word = rng();
    std::vector<Bits> sequential(inputs.size()), compiled(inputs.size()), word_xor(inputs.size());
    std::vector<double> sequential_ms, compiled_ms, warm_ms, word_ms;
    Transform::OperationCounts counts;
    const auto reference = [&] {
        for (std::size_t sample = 0; sample < inputs.size(); ++sample) {
            sequential[sample] = inputs[sample];
            reference_bits(sequential[sample], gates);
        }
    };
    const auto apply_batch = [&](const Transform& circuit) {
        for (std::size_t sample = 0; sample < inputs.size(); ++sample)
            circuit.apply(inputs[sample], compiled[sample]);
    };
    const Transform cached(width, gates);
    const auto warm = [&] { apply_batch(cached); };
    const auto candidate = [&] {
        const Transform circuit(width, gates);
        counts = circuit.operation_counts();
        apply_batch(circuit);
    };
    const auto direct = [&] {
        for (std::size_t sample = 0; sample < inputs.size(); ++sample) {
            word_xor[sample] = inputs[sample];
            for (std::size_t word = width / 128; word < width / 64; ++word)
                word_xor[sample][word] ^= inputs[sample][word - width / 128];
        }
    };
    reference();
    candidate();
    direct();
    for (unsigned i = 0; i < 5; ++i) {
        if (i % 2 == 0) {
            sequential_ms.push_back(timed(reference));
            compiled_ms.push_back(timed(candidate));
            warm_ms.push_back(timed(warm));
            word_ms.push_back(timed(direct));
        } else {
            word_ms.push_back(timed(direct));
            warm_ms.push_back(timed(warm));
            compiled_ms.push_back(timed(candidate));
            sequential_ms.push_back(timed(reference));
        }
        need(sequential == compiled && compiled == word_xor, "Resultado de transformación incorrecto");
    }
    std::cout << "xi_bits_entrada=" << width << '\n'
              << "xi_lotes_verificados=" << inputs.size() << '\n'
              << "xi_puertas_secuenciales=" << gates.size() << '\n'
              << "xi_terminos_de_palabra=" << counts.packed_terms << '\n'
              << "xi_filas_de_paridad=" << counts.parity_rows << '\n'
              << "xi_mediana_puertas_ms=" << median(sequential_ms) << '\n'
              << "xi_mediana_compilar_y_ejecutar_ms=" << median(compiled_ms) << '\n'
              << "xi_mediana_transformacion_reusada_ms=" << median(warm_ms) << '\n'
              << "xi_mediana_xor_cpu_directo_ms=" << median(word_ms) << '\n';
}

} // namespace

int main() {
    try {
        std::cout << std::fixed << std::setprecision(6);
        graphics_work();
        verify_against_xi();
        reversible_work();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
