#pragma once

#include "JNIUtils.h"
#include "Common/android/FilesystemAndroid.h"

class AndroidFilesystemCallbacks : public FilesystemAndroid::FilesystemCallbacks
{
    jmethodID m_openContentUriMid = nullptr;
    jmethodID m_listFilesMid = nullptr;
    jmethodID m_isDirectoryMid = nullptr;
    jmethodID m_isFileMid = nullptr;
    jmethodID m_existsMid = nullptr;
    JNIUtils::Scopedjclass m_fileUtilClass;

    bool CallBooleanFunction(const std::filesystem::path& uri, jmethodID methodId, const char* operation);

  public:
    AndroidFilesystemCallbacks();

    int OpenContentUri(const std::filesystem::path& uri) override;
    std::vector<std::filesystem::path> ListFiles(const std::filesystem::path& uri) override;
    bool IsDirectory(const std::filesystem::path& uri) override;
    bool IsFile(const std::filesystem::path& uri) override;
    bool Exists(const std::filesystem::path& uri) override;
};