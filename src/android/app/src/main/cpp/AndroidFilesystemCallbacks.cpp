#include "AndroidFilesystemCallbacks.h"
#include "Cemu/Logging/CemuLogging.h"

namespace
{
bool HasPendingJavaException(JNIEnv* env, const char* operation)
{
    if (!env->ExceptionCheck())
        return false;
    env->ExceptionClear();
    cemuLog_log(LogType::Force, "Android SAF/JNI: Java exception during {}; operation aborted", operation);
    return true;
}
}

AndroidFilesystemCallbacks::AndroidFilesystemCallbacks()
{
    JNIEnv* env = JNIUtils::GetEnv();
    m_fileUtilClass = JNIUtils::Scopedjclass("info/cemu/cemu/nativeinterface/NativeFiles");
    if (!*m_fileUtilClass)
    {
        HasPendingJavaException(env, "resolving NativeFiles class");
        cemuLog_log(LogType::Force, "Android SAF/JNI: NativeFiles class unavailable");
        return;
    }

    m_openContentUriMid = env->GetStaticMethodID(*m_fileUtilClass, "openContentUri", "(Ljava/lang/String;)I");
    m_listFilesMid = env->GetStaticMethodID(*m_fileUtilClass, "listFiles", "(Ljava/lang/String;)[Ljava/lang/String;");
    m_isDirectoryMid = env->GetStaticMethodID(*m_fileUtilClass, "isDirectory", "(Ljava/lang/String;)Z");
    m_isFileMid = env->GetStaticMethodID(*m_fileUtilClass, "isFile", "(Ljava/lang/String;)Z");
    m_existsMid = env->GetStaticMethodID(*m_fileUtilClass, "exists", "(Ljava/lang/String;)Z");
    if (HasPendingJavaException(env, "resolving NativeFiles methods"))
    {
        m_openContentUriMid = nullptr;
        m_listFilesMid = nullptr;
        m_isDirectoryMid = nullptr;
        m_isFileMid = nullptr;
        m_existsMid = nullptr;
    }
}

int AndroidFilesystemCallbacks::OpenContentUri(const std::filesystem::path& uri)
{
    int fd = -1;
    if (!m_fileUtilClass || !m_openContentUriMid)
    {
        cemuLog_log(LogType::Force, "Android SAF: openContentUri unavailable");
        return fd;
    }

    JNIUtils::FiberSafeJNICall([&](JNIEnv* env) {
        jstring uriString = JNIUtils::ToJString(env, uri);
        if (!uriString || HasPendingJavaException(env, "creating URI string"))
            return;
        fd = env->CallStaticIntMethod(*m_fileUtilClass, m_openContentUriMid, uriString);
        env->DeleteLocalRef(uriString);
        if (HasPendingJavaException(env, "openContentUri"))
            fd = -1;
        if (fd < 0)
            cemuLog_log(LogType::Force, "Android SAF: openContentUri failed (fd={})", fd);
    });
    return fd;
}

std::vector<std::filesystem::path> AndroidFilesystemCallbacks::ListFiles(const std::filesystem::path& uri)
{
    std::vector<std::filesystem::path> paths;
    if (!m_fileUtilClass || !m_listFilesMid)
    {
        cemuLog_log(LogType::Force, "Android SAF: listFiles unavailable");
        return paths;
    }

    JNIUtils::FiberSafeJNICall([&](JNIEnv* env) {
        jstring uriString = JNIUtils::ToJString(env, uri);
        if (!uriString || HasPendingJavaException(env, "creating list URI string"))
            return;

        jobjectArray array = static_cast<jobjectArray>(
            env->CallStaticObjectMethod(*m_fileUtilClass, m_listFilesMid, uriString));
        env->DeleteLocalRef(uriString);
        if (HasPendingJavaException(env, "listFiles"))
            return;
        if (!array)
        {
            cemuLog_log(LogType::Force, "Android SAF: listFiles returned null");
            return;
        }

        const jsize count = env->GetArrayLength(array);
        if (HasPendingJavaException(env, "reading listFiles result length"))
        {
            env->DeleteLocalRef(array);
            return;
        }

        paths.reserve(static_cast<size_t>(count));
        for (jsize i = 0; i < count; ++i)
        {
            jstring entry = static_cast<jstring>(env->GetObjectArrayElement(array, i));
            if (HasPendingJavaException(env, "reading listFiles entry"))
                break;
            if (!entry)
            {
                cemuLog_log(LogType::Force, "Android SAF: listFiles returned null entry at index {}", i);
                continue;
            }
            paths.push_back(JNIUtils::FromJString(env, entry));
            env->DeleteLocalRef(entry);
            if (HasPendingJavaException(env, "converting listFiles entry"))
                break;
        }
        env->DeleteLocalRef(array);
    });
    return paths;
}

bool AndroidFilesystemCallbacks::CallBooleanFunction(const std::filesystem::path& uri, jmethodID methodId, const char* operation)
{
    bool result = false;
    if (!m_fileUtilClass || !methodId)
    {
        cemuLog_log(LogType::Force, "Android SAF: {} unavailable", operation);
        return result;
    }

    JNIUtils::FiberSafeJNICall([&](JNIEnv* env) {
        jstring uriString = JNIUtils::ToJString(env, uri);
        if (!uriString || HasPendingJavaException(env, "creating query URI string"))
            return;
        result = env->CallStaticBooleanMethod(*m_fileUtilClass, methodId, uriString) == JNI_TRUE;
        env->DeleteLocalRef(uriString);
        if (HasPendingJavaException(env, operation))
            result = false;
    });
    return result;
}

bool AndroidFilesystemCallbacks::IsDirectory(const std::filesystem::path& uri)
{
    return CallBooleanFunction(uri, m_isDirectoryMid, "isDirectory");
}

bool AndroidFilesystemCallbacks::IsFile(const std::filesystem::path& uri)
{
    return CallBooleanFunction(uri, m_isFileMid, "isFile");
}

bool AndroidFilesystemCallbacks::Exists(const std::filesystem::path& uri)
{
    return CallBooleanFunction(uri, m_existsMid, "exists");
}