#include "android_boot_session.hpp"

#include <jni.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {
using Session = reverse_windows::AndroidBootSession;
constexpr jint kVideoDirty = 0x100; // Kept in sync with NativeMachine.STEP_VIDEO_DIRTY.

struct Instance {
    explicit Instance(std::unique_ptr<Session> machine) : machine(std::move(machine)) {}
    std::mutex mutex;
    std::unique_ptr<Session> machine;
};

std::mutex registry_mutex;
std::unordered_map<jlong, std::shared_ptr<Instance>> registry;
jlong next_handle = 1;

void throw_java(JNIEnv* env, const char* type, const char* message) {
    if (env->ExceptionCheck()) return;
    jclass error = env->FindClass(type);
    if (error) env->ThrowNew(error, message);
}

template <typename Return, typename Action>
Return call(JNIEnv* env, Return error_value, Action action) {
    try {
        return action();
    } catch (const std::invalid_argument& error) {
        throw_java(env, "java/lang/IllegalArgumentException", error.what());
    } catch (const std::bad_alloc&) {
        throw_java(env, "java/lang/OutOfMemoryError", "Memoria insuficiente para la máquina.");
    } catch (const std::exception& error) {
        throw_java(env, "java/lang/IllegalStateException", error.what());
    } catch (...) {
        throw_java(env, "java/lang/IllegalStateException", "Fallo nativo desconocido.");
    }
    return error_value;
}

std::string string_from(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) throw std::bad_alloc();
    struct Release {
        JNIEnv* env;
        jstring value;
        const char* chars;
        ~Release() { env->ReleaseStringUTFChars(value, chars); }
    } release{env, value, chars};
    return chars;
}

template <typename Action>
auto session(jlong handle, Action action) {
    std::shared_ptr<Instance> instance;
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        const auto it = registry.find(handle);
        if (it == registry.end()) throw std::invalid_argument("La máquina está cerrada.");
        instance = it->second;
    }
    std::lock_guard<std::mutex> lock(instance->mutex);
    if (!instance->machine) throw std::invalid_argument("La máquina está cerrada.");
    return action(*instance->machine);
}

Session::RamProfile ram_profile(jlong available, jlong threshold, jboolean low_memory,
                                jboolean prime_ram) {
    const bool have_memory_info = available != -1;
    if (!have_memory_info) {
        available = 2 * Session::kDefaultRamProfile.backing_bytes;
        threshold = 0;
    }
    if (available < 0 || threshold < 0)
        throw std::invalid_argument("Android devolvió un perfil de RAM inválido.");
    auto profile = Session::ram_profile(static_cast<std::uint64_t>(available),
            static_cast<std::uint64_t>(threshold), low_memory == JNI_TRUE, prime_ram == JNI_TRUE);
    if (!have_memory_info) profile.backing_bytes = Session::kDefaultRamProfile.backing_bytes;
    return profile;
}
} // namespace

extern "C" {

JNIEXPORT jstring JNICALL Java_com_reversewindows_android_NativeMachine_checkHardwareWithRam(
        JNIEnv* env, jclass, jstring disk, jint iso_fd, jlong iso_bytes, jstring iso_error,
        jlong available, jlong threshold, jboolean low_memory, jboolean prime_ram, jstring ram_cache) {
    return call<jstring>(env, nullptr, [&] {
        if (iso_bytes < 0) throw std::invalid_argument("Tamaño de medio inválido.");
        const auto result = Session::check_hardware(string_from(env, disk), iso_fd,
                static_cast<std::uint64_t>(iso_bytes), string_from(env, iso_error),
                ram_profile(available, threshold, low_memory, prime_ram),
                string_from(env, ram_cache), Session::kPrimeCpuTopology);
        return env->NewStringUTF(result.c_str());
    });
}

JNIEXPORT jlong JNICALL Java_com_reversewindows_android_NativeMachine_openWithRam(
        JNIEnv* env, jclass, jstring disk, jint capacity, jint iso_fd, jlong iso_bytes, jint source,
        jlong available, jlong threshold, jboolean low_memory, jboolean prime_ram, jstring ram_cache,
        jstring graphics_vram, jstring data_disk) {
    return call<jlong>(env, 0, [&] {
        if (capacity < 0 || iso_bytes < 0)
            throw std::invalid_argument("Tamaño de medio inválido.");
        auto machine = std::make_unique<Session>(
            string_from(env, disk), static_cast<unsigned>(capacity),
            static_cast<Session::BootSource>(source), iso_fd,
            static_cast<std::uint64_t>(iso_bytes), ram_profile(available, threshold, low_memory, prime_ram),
            string_from(env, ram_cache), string_from(env, graphics_vram), string_from(env, data_disk),
            Session::kPrimeCpuTopology, true, Session::kPrimeCpuClock);
        auto instance = std::make_shared<Instance>(std::move(machine));
        std::lock_guard<std::mutex> lock(registry_mutex);
        if (next_handle == std::numeric_limits<jlong>::max())
            throw std::overflow_error("Se agotaron los identificadores de máquina.");
        const jlong handle = next_handle++;
        registry.emplace(handle, std::move(instance));
        return handle;
    });
}

JNIEXPORT void JNICALL Java_com_reversewindows_android_NativeMachine_close(
        JNIEnv* env, jclass, jlong handle) {
    call(env, false, [&] {
        std::shared_ptr<Instance> instance;
        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            const auto it = registry.find(handle);
            if (it == registry.end()) return false;
            instance = std::move(it->second);
            registry.erase(it);
        }
        std::lock_guard<std::mutex> lock(instance->mutex);
        instance->machine.reset();
        return true;
    });
}

JNIEXPORT void JNICALL Java_com_reversewindows_android_NativeMachine_createDisk(
        JNIEnv* env, jclass, jstring disk, jint capacity) {
    call(env, false, [&] {
        if (capacity <= 0) throw std::invalid_argument("Capacidad QSSD inválida.");
        Session::create_disk(string_from(env, disk), static_cast<unsigned>(capacity));
        return true;
    });
}

JNIEXPORT jlong JNICALL Java_com_reversewindows_android_NativeMachine_step(
        JNIEnv* env, jclass, jlong handle, jint budget, jlong hostBudgetNs) {
    return call<jlong>(env, 3, [&] {
        if (budget <= 0 || hostBudgetNs <= 0) throw std::invalid_argument("Presupuesto de ejecución inválido.");
        return session(handle, [budget, hostBudgetNs](Session& machine) {
            using Clock = cosmic_pc::PcMachine::RunClock;
            const auto now = Clock::now();
            const auto duration = std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(hostBudgetNs));
            const auto deadline = now + std::min(duration, Clock::time_point::max() - now);
            const auto before = machine.session_steps();
            const auto result = machine.step(static_cast<unsigned>(budget), deadline);
            const auto outcome = result == Session::Step::Halted && machine.is_stopped()
                ? jint{4} : static_cast<jint>(result);
            const auto flags = outcome | (machine.video_pending() ? kVideoDirty : 0);
            const auto completed = machine.session_steps() - before;
            return static_cast<jlong>((completed << 32) | static_cast<std::uint32_t>(flags));
        });
    });
}

JNIEXPORT void JNICALL Java_com_reversewindows_android_NativeMachine_setExperimentalClock(
        JNIEnv* env, jclass, jlong handle, jboolean enabled) {
    call(env, false, [&] {
        return session(handle, [=](Session& machine) {
            machine.set_experimental_tsc(enabled == JNI_TRUE);
            return true;
        });
    });
}

JNIEXPORT jintArray JNICALL Java_com_reversewindows_android_NativeMachine_cpuThreadIds(
        JNIEnv* env, jclass, jlong handle) {
    return call<jintArray>(env, nullptr, [&] {
        return session(handle, [env](Session& machine) {
            const auto tids = machine.host_thread_ids();
            const auto count = static_cast<jsize>(tids.size());
            auto array = env->NewIntArray(count);
            if (array) env->SetIntArrayRegion(array, 0, count, tids.data());
            return array;
        });
    });
}

JNIEXPORT jboolean JNICALL Java_com_reversewindows_android_NativeMachine_key(
        JNIEnv* env, jclass, jlong handle, jint ascii, jint scancode, jboolean pressed) {
    return call<jboolean>(env, JNI_FALSE, [&] {
        return session(handle, [=](Session& machine) -> jboolean {
            return machine.key(static_cast<unsigned>(ascii), static_cast<unsigned>(scancode),
                               pressed != JNI_FALSE) ? JNI_TRUE : JNI_FALSE;
        });
    });
}

JNIEXPORT jboolean JNICALL Java_com_reversewindows_android_NativeMachine_sampleRamPressure(
        JNIEnv* env, jclass, jlong handle, jlong available, jlong threshold, jboolean low_memory) {
    return call<jboolean>(env, JNI_FALSE, [&] {
        if (available < 0 || threshold < 0)
            throw std::invalid_argument("Android devolvió un perfil de RAM inválido.");
        return session(handle, [=](Session& machine) -> jboolean {
            machine.sample_ram_pressure(static_cast<std::uint64_t>(available),
                    static_cast<std::uint64_t>(threshold), low_memory != JNI_FALSE);
            return machine.maintain_ram_budget() ? JNI_TRUE : JNI_FALSE;
        });
    });
}

JNIEXPORT jboolean JNICALL Java_com_reversewindows_android_NativeMachine_maintainRamBudget(
        JNIEnv* env, jclass, jlong handle) {
    return call<jboolean>(env, JNI_FALSE, [&] {
        return session(handle, [](Session& machine) -> jboolean {
            return machine.maintain_ram_budget() ? JNI_TRUE : JNI_FALSE;
        });
    });
}

JNIEXPORT jboolean JNICALL Java_com_reversewindows_android_NativeMachine_prepareRamReclaim(
        JNIEnv* env, jclass, jlong handle) {
    return call<jboolean>(env, JNI_FALSE, [&] {
        return session(handle, [](Session& machine) -> jboolean {
            return machine.prepare_ram_reclaim() ? JNI_TRUE : JNI_FALSE;
        });
    });
}

JNIEXPORT void JNICALL Java_com_reversewindows_android_NativeMachine_flush(
        JNIEnv* env, jclass, jlong handle) {
    call(env, false, [&] {
        return session(handle, [](Session& machine) {
            machine.flush();
            return true;
        });
    });
}

JNIEXPORT jstring JNICALL Java_com_reversewindows_android_NativeMachine_diagnostic(
        JNIEnv* env, jclass, jlong handle, jboolean detailed) {
    return call<jstring>(env, nullptr, [&] {
        return session(handle, [env, detailed](Session& machine) {
            return env->NewStringUTF(machine.diagnostic(detailed == JNI_TRUE).c_str());
        });
    });
}

JNIEXPORT jintArray JNICALL Java_com_reversewindows_android_NativeMachine_captureVideo(
        JNIEnv* env, jclass, jlong handle, jbyteArray text) {
    return call<jintArray>(env, nullptr, [&] {
        std::array<std::uint8_t, 4000> cells{};
        if (!text || env->GetArrayLength(text) < static_cast<jsize>(cells.size()))
            throw std::invalid_argument("Buffer VGA insuficiente.");
        return session(handle, [&](Session& machine) -> jintArray {
            try {
                const auto frame = machine.capture_video(cells.data(), cells.size());
                if (!frame.graphics && !frame.text_bytes) return nullptr;
                const jint dimensions[]{static_cast<jint>(frame.width), static_cast<jint>(frame.height),
                                         static_cast<jint>(xgpu::FramebufferWorkAuthority::kStripeHeight)};
                const auto words = frame.graphics ? frame.graphics->size() : 0;
                const auto header = frame.graphics ? 3 : 2;
                if (words > std::size_t(std::numeric_limits<jsize>::max()) - header)
                    throw std::length_error("Actualización de video demasiado grande.");
                auto result = env->NewIntArray(static_cast<jsize>(words + header));
                if (!result) {
                    machine.retry_video();
                    return nullptr;
                }
                if (frame.graphics) {
                    auto* out = env->GetPrimitiveArrayCritical(result, nullptr);
                    if (!out) {
                        machine.retry_video();
                        return nullptr;
                    }
                    static_assert(sizeof(jint) == sizeof(std::uint32_t), "JNI color size");
                    std::memcpy(out, dimensions, sizeof(dimensions));
                    std::memcpy(static_cast<jint*>(out) + header, frame.graphics->data(),
                                words * sizeof(jint));
                    env->ReleasePrimitiveArrayCritical(result, out, 0);
                } else {
                    env->SetIntArrayRegion(result, 0, 2, dimensions);
                    env->SetByteArrayRegion(text, 0, static_cast<jsize>(frame.text_bytes),
                                            reinterpret_cast<const jbyte*>(cells.data()));
                }
                if (env->ExceptionCheck()) {
                    machine.retry_video();
                    return nullptr;
                }
                return result;
            } catch (...) {
                machine.retry_video();
                throw;
            }
        });
    });
}

JNIEXPORT void JNICALL Java_com_reversewindows_android_NativeMachine_retryVideo(
        JNIEnv* env, jclass, jlong handle) {
    call(env, false, [&] {
        return session(handle, [](Session& machine) { machine.retry_video(); return true; });
    });
}

} // extern "C"
