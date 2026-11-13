#include "android_boot_session.hpp"

#include <jni.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {
using Session = reverse_windows::AndroidBootSession;

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
} // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_com_reversewindows_android_NativeMachine_open(
        JNIEnv* env, jclass, jstring disk, jint capacity, jint iso_fd, jlong iso_bytes, jint source) {
    return call<jlong>(env, 0, [&] {
        if (capacity < 0 || iso_bytes < 0)
            throw std::invalid_argument("Tamaño de medio inválido.");
        auto machine = std::make_unique<Session>(
            string_from(env, disk), static_cast<unsigned>(capacity),
            static_cast<Session::BootSource>(source), iso_fd,
            static_cast<std::uint64_t>(iso_bytes));
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

JNIEXPORT jint JNICALL Java_com_reversewindows_android_NativeMachine_step(
        JNIEnv* env, jclass, jlong handle, jint budget) {
    return call<jint>(env, 3, [&] {
        if (budget <= 0) throw std::invalid_argument("Presupuesto de ejecución inválido.");
        return session(handle, [budget](Session& machine) {
            const auto result = machine.step(static_cast<unsigned>(budget));
            return result == Session::Step::Halted && machine.is_stopped()
                ? jint{4} : static_cast<jint>(result);
        });
    });
}

JNIEXPORT void JNICALL Java_com_reversewindows_android_NativeMachine_key(
        JNIEnv* env, jclass, jlong handle, jint ascii, jint scancode, jboolean pressed) {
    call(env, false, [&] {
        return session(handle, [=](Session& machine) {
            machine.key(static_cast<unsigned>(ascii), static_cast<unsigned>(scancode), pressed != JNI_FALSE);
            return true;
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
        JNIEnv* env, jclass, jlong handle) {
    return call<jstring>(env, nullptr, [&] {
        return session(handle, [env](Session& machine) {
            return env->NewStringUTF(machine.diagnostic().c_str());
        });
    });
}

JNIEXPORT jintArray JNICALL Java_com_reversewindows_android_NativeMachine_videoInfo(
        JNIEnv* env, jclass, jlong handle) {
    return call<jintArray>(env, nullptr, [&] {
        return session(handle, [env](Session& machine) {
            const auto info = machine.video_info();
            const jint dimensions[]{static_cast<jint>(info.width), static_cast<jint>(info.height),
                                    info.graphics ? 1 : 0};
            jintArray result = env->NewIntArray(3);
            if (result) env->SetIntArrayRegion(result, 0, 3, dimensions);
            return result;
        });
    });
}

JNIEXPORT jint JNICALL Java_com_reversewindows_android_NativeMachine_copyText(
        JNIEnv* env, jclass, jlong handle, jbyteArray result) {
    return call<jint>(env, 0, [&] {
        std::array<std::uint8_t, 4000> bytes{};
        if (!result || env->GetArrayLength(result) < static_cast<jsize>(bytes.size()))
            throw std::invalid_argument("Buffer VGA insuficiente.");
        const auto count = session(handle, [&](Session& machine) {
            return machine.copy_text(bytes.data(), bytes.size());
        });
        if (count) env->SetByteArrayRegion(result, 0, static_cast<jsize>(count),
                                           reinterpret_cast<const jbyte*>(bytes.data()));
        return static_cast<jint>(count);
    });
}

JNIEXPORT void JNICALL Java_com_reversewindows_android_NativeMachine_copyGraphics(
        JNIEnv* env, jclass, jlong handle, jint width, jint height, jintArray result) {
    call(env, false, [&] {
        if (!result || width <= 0 || height <= 0)
            throw std::invalid_argument("Buffer VBE inválido.");
        const auto capacity = static_cast<std::size_t>(env->GetArrayLength(result));
        return session(handle, [&](Session& machine) {
            jint* pixels = env->GetIntArrayElements(result, nullptr);
            if (!pixels) throw std::bad_alloc();
            try {
                static_assert(sizeof(jint) == sizeof(std::uint32_t), "JNI color size");
                machine.copy_graphics(reinterpret_cast<std::uint32_t*>(pixels), capacity,
                                      static_cast<unsigned>(width), static_cast<unsigned>(height));
            } catch (...) {
                env->ReleaseIntArrayElements(result, pixels, JNI_ABORT);
                throw;
            }
            env->ReleaseIntArrayElements(result, pixels, 0);
            return true;
        });
    });
}

} // extern "C"
