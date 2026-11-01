#include "AndroidFilesystemCallbacks.h"
#include "AndroidInputHelpers.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/TitleList/TitleId.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "Cemu/Logging/CemuLogging.h"
#include "config/ActiveSettings.h"
#include "input/InputManager.h"
#include "JNIUtils.h"
#include "WindowSystem.h"

#if HAS_CUBEB
#include "audio/CubebAPI.h"
#endif // HAS_CUBEB

#include <android/native_window_jni.h>
#include <chrono>
#include <deque>
#include <fstream>
#include <mutex>

// forward declaration from main.cpp
void CemuCommonInit();

namespace NativeEmulation
{

    class AndroidLogCapture final : public LoggingCallbacks
    {
    public:
        void Log(std::string_view filter, std::string_view message) override
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - s_startTime).count();

            std::string line = fmt::format("[+{}.{:03d}s] ", elapsed / 1000, elapsed % 1000);
            if (!filter.empty())
            {
                line.append("[");
                line.append(filter);
                line.append("] ");
            }
            line.append(message);
            line.push_back('\n');
            if (line.size() > kMaxBytes - 1024)
            {
                line.resize(kMaxBytes - 1024);
                line.append("\n[diagnostic line truncated at capture size limit]\n");
            }

            std::lock_guard lock(m_mutex);
            m_lines.emplace_back(line);
            m_totalBytes += line.size();
            while (m_totalBytes > kMaxBytes && !m_lines.empty())
            {
                m_totalBytes -= m_lines.front().size();
                m_lines.pop_front();
                m_wasTrimmed = true;
            }

            if (m_file.is_open())
            {
                RotateIfNeeded(line.size());
                if (m_file.is_open())
                {
                    m_file.write(line.data(), static_cast<std::streamsize>(line.size()));
                    m_file.flush();
                    if (m_file)
                        m_fileBytes += line.size();
                    else
                        m_fileError = "write/flush failed";
                }
            }
        }

        void Log(std::string_view filter, std::wstring_view message) override
        {
            Log(filter, boost::nowide::narrow(message));
        }

        void StartPersistentCapture(const fs::path& path)
        {
            std::lock_guard lock(m_mutex);
            m_persistentPath = path;
            m_previousPath = path;
            m_previousPath += ".previous";

            std::error_code error;
            fs::create_directories(path.parent_path(), error);
            if (error)
                m_fileError = fmt::format("cannot create log directory: {}", error.message());

            error.clear();
            if (fs::exists(path, error))
            {
                error.clear();
                m_fileBytes = fs::file_size(path, error);
                if (error)
                {
                    m_fileBytes = 0;
                    m_fileError = fmt::format("cannot inspect existing log: {}", error.message());
                }
            }

            if (m_fileBytes > kMaxBytes)
                RotateExistingFile();

            m_file.open(m_persistentPath, std::ios::out | std::ios::app | std::ios::binary);
            if (!m_file.is_open())
            {
                m_fileError = "cannot open persistent diagnostic log";
                return;
            }

            const std::string session = "\n===== Android diagnostic capture session started =====\n";
            RotateIfNeeded(session.size());
            if (m_file.is_open())
            {
                m_file.write(session.data(), static_cast<std::streamsize>(session.size()));
                m_file.flush();
                if (m_file)
                    m_fileBytes += session.size();
                else
                    m_fileError = "failed to write session header";
            }
        }

        std::string Snapshot()
        {
            std::lock_guard lock(m_mutex);
            if (m_file.is_open())
                m_file.flush();

            std::string result;
            result.reserve(std::min<size_t>(m_fileBytes + 1024, kMaxBytes * 2 + 4096));
            AppendFileIfPresent(m_previousPath, result);
            AppendFileIfPresent(m_persistentPath, result);

            if (!result.empty())
            {
                if (!m_fileError.empty())
                    result.append("\n[DIAGNOSTIC CAPTURE WARNING] ").append(m_fileError).append("\n");
                return result;
            }

            result.reserve(m_totalBytes + 256);
            result.append("Persistent log file unavailable; showing bounded in-memory capture only.\n");
            if (!m_fileError.empty())
                result.append("[DIAGNOSTIC CAPTURE WARNING] ").append(m_fileError).append("\n");
            if (m_wasTrimmed)
                result.append("[WARNING] Older in-memory log entries were discarded at the 32 MiB limit.\n");
            for (const auto& line : m_lines)
                result.append(line);
            return result;
        }

    private:
        static constexpr size_t kMaxBytes = 32 * 1024 * 1024;
        static const std::chrono::steady_clock::time_point s_startTime;
        std::mutex m_mutex;
        std::deque<std::string> m_lines;
        fs::path m_persistentPath;
        fs::path m_previousPath;
        std::ofstream m_file;
        size_t m_totalBytes = 0;
        uint64 m_fileBytes = 0;
        bool m_wasTrimmed = false;
        std::string m_fileError;

        void AppendFileIfPresent(const fs::path& path, std::string& output)
        {
            if (path.empty())
                return;
            std::ifstream input(path, std::ios::binary);
            if (input.is_open())
                output.append(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }

        void RotateExistingFile()
        {
            std::error_code error;
            if (fs::exists(m_previousPath, error))
            {
                error.clear();
                fs::remove(m_previousPath, error);
            }
            error.clear();
            fs::rename(m_persistentPath, m_previousPath, error);
            if (error)
            {
                m_fileError = fmt::format("log rotation failed: {}", error.message());
                error.clear();
                fs::remove(m_persistentPath, error);
            }
            m_fileBytes = 0;
        }

        void RotateIfNeeded(size_t incomingBytes)
        {
            if (m_fileBytes + incomingBytes <= kMaxBytes)
                return;

            if (m_file.is_open())
                m_file.close();
            RotateExistingFile();
            m_file.clear();
            m_file.open(m_persistentPath, std::ios::out | std::ios::trunc | std::ios::binary);
            if (!m_file.is_open())
            {
                m_fileError = "cannot reopen diagnostic log after rotation";
                return;
            }

            const std::string marker = "===== Previous diagnostic segment rotated; older entries are in .previous =====\n";
            m_file.write(marker.data(), static_cast<std::streamsize>(marker.size()));
            m_file.flush();
            if (m_file)
                m_fileBytes = marker.size();
            else
                m_fileError = "failed to write log rotation marker";
        }
    };

    const std::chrono::steady_clock::time_point AndroidLogCapture::s_startTime = std::chrono::steady_clock::now();

    AndroidLogCapture s_androidLogCapture;

    std::string GetVulkanDiagnosticLog()
    {
        std::string report;
        report.reserve(1024 * 1024);
        report.append("Cemu Android diagnostic report\n");
        report.append("===============================\n");
        report.append("Capture source: Cemu native LoggingCallbacks (not Android-wide Logcat).\n");
        report.append("Persistent log: app-private vulkan-diagnostic.log with one rotated .previous segment.\n");
        report.append("Each active/previous segment is bounded to 32 MiB; export combines both when available.\n");
        report.append("Vulkan validation requested: ");
        report.append(cemuLog_isLoggingEnabled(LogType::VulkanValidation) ? "yes\n" : "no\n");
        report.append("Check session entries for actual validation-layer and debug-messenger initialization status.\n\n");
        report.append(s_androidLogCapture.Snapshot());
        if (report.empty() || report.back() != '\n')
            report.push_back('\n');
        return report;
    }

	void CreateAudioDevice(IAudioAPI::AudioAPI audioApi, AudioChannels channels, sint32 volume, bool isTV)
	{
		constexpr int AX_FRAMES_PER_GROUP = 4;
		std::unique_lock lock(g_audioMutex);
		auto& audioDevice = isTV ? g_tvAudio : g_padAudio;

		switch (audioApi)
		{
#if HAS_CUBEB
		case IAudioAPI::AudioAPI::Cubeb:
		{
			audioDevice.reset();
			auto deviceDescriptionPtr = std::make_shared<CubebAPI::CubebDeviceDescription>(nullptr, std::string(), std::wstring());
			audioDevice = IAudioAPI::CreateDevice(
				IAudioAPI::AudioAPI::Cubeb,
				deviceDescriptionPtr,
				48000,
				CemuConfig::AudioChannelsToNChannels(channels),
				snd_core::AX_SAMPLES_PER_3MS_48KHZ * AX_FRAMES_PER_GROUP,
				16);
			audioDevice->SetVolume(volume);
			break;
		}
#endif // HAS_CUBEB
		default:
			cemuLog_log(LogType::Force, "Invalid audio api: {}", audioApi);
			break;
		}
	}

	void InitializeAudioDevices()
	{
		auto& config = GetConfig();
		if (!config.tv_device.empty())
			CreateAudioDevice(IAudioAPI::AudioAPI::Cubeb, config.tv_channels, config.tv_volume, true);

		if (!config.pad_device.empty())
			CreateAudioDevice(IAudioAPI::AudioAPI::Cubeb, config.pad_channels, config.pad_volume, false);
	}

	void SetDefaultDeviceController()
	{
		for (size_t i = 0; i < InputManager::kMaxController; ++i)
		{
			auto emulatedController = InputManager::instance().get_controller(i);

			if (!emulatedController)
			{
				continue;
			}

			for (const auto& controller : emulatedController->get_controllers())
			{
				if (controller->api() == InputAPI::Device)
				{
					return;
				}
			}
		}

		if (auto emulatedController = InputManager::instance().get_controller(0))
		{
			auto controller = CreateDefaultDeviceController();
			emulatedController->add_controller(controller);
		}
	}

	void CreateCemuDirectories()
	{
		const auto mlc = ActiveSettings::GetMlcPath();

		// create sys/usr folder in mlc01
		const auto sysFolder = mlc / "sys";
		fs::create_directories(sysFolder);

		const auto usrFolder = mlc / "usr";
		fs::create_directories(usrFolder);
		fs::create_directories(usrFolder / "title/00050000"); // base
		fs::create_directories(usrFolder / "title/0005000c"); // dlc
		fs::create_directories(usrFolder / "title/0005000e"); // update

		// Mii Maker save folders {0x500101004A000, 0x500101004A100, 0x500101004A200},
		fs::create_directories(mlc / "usr/save/00050010/1004a100/user/common/db");
		fs::create_directories(mlc / "usr/save/00050010/1004a000/user/common/db");
		fs::create_directories(mlc / "usr/save/00050010/1004a200/user/common/db");

		// lang files
		auto langDir = mlc / "sys/title/0005001b/1005c000/content";
		fs::create_directories(langDir);

		auto langFile = langDir / "language.txt";

		if (!fs::exists(langFile))
		{
			if (std::ofstream file{langFile})
			{
				const auto langStrings = {"ja", "en", "fr", "de", "it", "es", "ko", "nl", "pt", "ru", "zh"};
				for (const char* lang : langStrings)
					fmt::println(file, R"("{}",)", lang);
			}
		}

		auto countryFile = langDir / "country.txt";
		if (!fs::exists(countryFile))
		{
			if (std::ofstream file{countryFile})
			{
				for (sint32 i = 0; i < 201; i++)
				{
					const char* countryCode = NCrypto::GetCountryAsString(i);
					if (boost::iequals(countryCode, "NN"))
						fmt::println(file, "NULL,");
					else
						fmt::println(file, R"("{}",)", countryCode);
				}
			}
		}

		// cemu directories
		fs::create_directories(ActiveSettings::GetConfigPath("controllerProfiles"));
		fs::create_directories(ActiveSettings::GetUserDataPath("memorySearcher"));
	}

	enum PrepareTitleResult : sint32
	{
		SUCCESSFUL = 0,
		ERROR_GAME_BASE_FILES_NOT_FOUND = 1,
		ERROR_NO_DISC_KEY = 2,
		ERROR_NO_TITLE_TIK = 3,
		ERROR_UNKNOWN = 4,
	};

	class TestSurface
	{
	  public:
		TestSurface()
		{
			JNIEnv* env = JNIUtils::GetEnv();

			jclass surfaceTextureClass = env->FindClass("android/graphics/SurfaceTexture");
			jmethodID ctorSurfaceTexture = env->GetMethodID(surfaceTextureClass, "<init>", "(I)V");
			jobject localSurfaceTexture = env->NewObject(surfaceTextureClass, ctorSurfaceTexture, 0);

			jclass surfaceClass = env->FindClass("android/view/Surface");
			jmethodID ctorSurface = env->GetMethodID(surfaceClass, "<init>", "(Landroid/graphics/SurfaceTexture;)V");
			jobject localSurface = env->NewObject(surfaceClass, ctorSurface, localSurfaceTexture);

			m_surfaceTexture = env->NewGlobalRef(localSurfaceTexture);
			m_surface = env->NewGlobalRef(localSurface);

			m_window = ANativeWindow_fromSurface(env, m_surface);
			ANativeWindow_acquire(m_window);

			env->DeleteLocalRef(localSurfaceTexture);
			env->DeleteLocalRef(localSurface);
			env->DeleteLocalRef(surfaceTextureClass);
			env->DeleteLocalRef(surfaceClass);
		}

		~TestSurface()
		{
			JNIEnv* env = JNIUtils::GetEnv();

			ANativeWindow_release(m_window);

			jclass surfaceClass = env->FindClass("android/view/Surface");
			jmethodID releaseSurface = env->GetMethodID(surfaceClass, "release", "()V");
			env->CallVoidMethod(m_surface, releaseSurface);
			env->DeleteGlobalRef(m_surface);
			m_surface = nullptr;
			env->DeleteLocalRef(surfaceClass);

			jclass surfaceTextureClass = env->FindClass("android/graphics/SurfaceTexture");
			jmethodID releaseSurfaceTexture = env->GetMethodID(surfaceTextureClass, "release", "()V");
			env->CallVoidMethod(m_surfaceTexture, releaseSurfaceTexture);
			env->DeleteGlobalRef(m_surfaceTexture);
			m_surfaceTexture = nullptr;
			env->DeleteLocalRef(surfaceTextureClass);
		}

		ANativeWindow* getWindow()
		{
			return m_window;
		}

	  private:
		ANativeWindow* m_window;
		jobject m_surface = nullptr;
		jobject m_surfaceTexture = nullptr;
	};
} // namespace NativeEmulation

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_setReplaceTVWithPadView([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jboolean swapped)
{
	// Emulate pressing the TAB key for showing DRC instead of TV
	WindowSystem::GetWindowInfo().set_keystate(static_cast<uint32>(WindowSystem::PlatformKeyCodes::TAB), swapped);
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_initializeEmulation([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	FilesystemAndroid::SetFilesystemCallbacks(std::make_shared<AndroidFilesystemCallbacks>());
	GetConfigHandle().SetFilename(ActiveSettings::GetConfigPath("settings.xml").generic_wstring());
	NativeEmulation::CreateCemuDirectories();
	NetworkConfig::LoadOnce();
	ActiveSettings::Init();
	NativeEmulation::s_androidLogCapture.StartPersistentCapture(ActiveSettings::GetUserDataPath("vulkan-diagnostic.log"));
	cemuLog_setCallbacks(&NativeEmulation::s_androidLogCapture);
	LatteOverlay_init();
	CemuCommonInit();
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_initializeRenderer(JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	static std::unique_ptr<NativeEmulation::TestSurface> testSurface;

	InitializeGlobalVulkan();
	JNIUtils::HandleNativeException(env, [&]() {
		testSurface = std::make_unique<NativeEmulation::TestSurface>();

		WindowSystem::GetWindowInfo().window_main.surface = testSurface->getWindow();

		g_renderer = std::make_unique<VulkanRenderer>();
	});
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_setDPI([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jfloat dpi)
{
	auto& windowInfo = WindowSystem::GetWindowInfo();
	windowInfo.dpi_scale = windowInfo.pad_dpi_scale = dpi;
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_clearPadSurface([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	VulkanRenderer::GetInstance()->StopUsingPadAndWait();
	WindowSystem::GetWindowInfo().pad_open = false;
}

extern "C" [[maybe_unused]] JNIEXPORT jboolean JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_supportsLoadingCustomDriver([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	return SupportsLoadingCustomDriver();
}

extern "C" [[maybe_unused]] JNIEXPORT jstring JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_getVulkanDiagnosticLog(JNIEnv* env, [[maybe_unused]] jclass clazz)
{
    return JNIUtils::ToJString(env, NativeEmulation::GetVulkanDiagnosticLog());
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_setVulkanValidationEnabled([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jboolean enabled)
{
    uint64 flags = s_loggingFlagMask.load(std::memory_order_relaxed);
    const uint64 validationFlag = cemuLog_getFlag(LogType::VulkanValidation);
    if (enabled)
        flags |= validationFlag;
    else
        flags &= ~validationFlag;
    cemuLog_setActiveLoggingFlags(flags);
}

extern "C" [[maybe_unused]] JNIEXPORT jboolean JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_isVulkanValidationEnabled([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
    return cemuLog_isLoggingEnabled(LogType::VulkanValidation) ? JNI_TRUE : JNI_FALSE;
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_setSurface(JNIEnv* env, [[maybe_unused]] jclass clazz, jobject surface, jboolean isMainCanvas)
{
	JNIUtils::HandleNativeException(env, [&]() {
		auto& windowHandleInfo = isMainCanvas ? WindowSystem::GetWindowInfo().canvas_main : WindowSystem::GetWindowInfo().canvas_pad;
		auto oldWindow = windowHandleInfo.surface.load();
		if (oldWindow != nullptr)
			ANativeWindow_release(static_cast<ANativeWindow*>(oldWindow));
		auto newSurface = ANativeWindow_fromSurface(env, surface);
		ANativeWindow_acquire(newSurface);
		windowHandleInfo.surface = newSurface;
		windowHandleInfo.surface.notify_all();
	});
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_initializeSurface(JNIEnv* env, [[maybe_unused]] jclass clazz, jboolean isMainCanvas)
{
	JNIUtils::HandleNativeException(env, [&]() {
		int width, height;
		if (isMainCanvas)
		{
			WindowSystem::GetWindowPhysSize(width, height);
		}
		else
		{
			WindowSystem::GetPadWindowPhysSize(width, height);
			WindowSystem::GetWindowInfo().pad_open = true;
		}

		VulkanRenderer::GetInstance()->InitializeSurface({width, height}, isMainCanvas);
	});
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_setSurfaceSize([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jint width, jint height, jboolean isMainCanvas)
{
	auto& windowInfo = WindowSystem::GetWindowInfo();
	if (isMainCanvas)
	{
		windowInfo.width = windowInfo.phys_width = width;
		windowInfo.height = windowInfo.phys_height = height;
	}
	else
	{
		windowInfo.pad_width = windowInfo.phys_pad_width = width;
		windowInfo.pad_height = windowInfo.phys_pad_height = height;
	}
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_initializeSystems([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	NativeEmulation::SetDefaultDeviceController();
	WindowSystem::GetWindowInfo().set_keystatesup();
	NativeEmulation::InitializeAudioDevices();
}

extern "C" [[maybe_unused]] JNIEXPORT jint JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_prepareTitle([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jstring launchPathJava)
{
	fs::path launchPath = JNIUtils::FromJString(env, launchPathJava);

	TitleInfo launchTitle{launchPath};

	using enum NativeEmulation::PrepareTitleResult;

	if (launchTitle.IsValid())
	{
		// the title might not be in the TitleList, so we add it as a temporary entry
		CafeTitleList::AddTitleFromPath(launchPath);
		// title is valid, launch from TitleId
		TitleId baseTitleId;
		if (!CafeTitleList::FindBaseTitleId(launchTitle.GetAppTitleId(), baseTitleId))
		{
			return ERROR_GAME_BASE_FILES_NOT_FOUND;
		}
		CafeSystem::PREPARE_STATUS_CODE r = CafeSystem::PrepareForegroundTitle(baseTitleId);
		if (r != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
		{
			return ERROR_UNKNOWN;
		}
	}
	else // if (launchTitle.GetFormat() == TitleInfo::TitleDataFormat::INVALID_STRUCTURE )
	{
		// title is invalid, if it's an RPX/ELF we can launch it directly
		// otherwise it's an error
		CafeTitleFileType fileType = DetermineCafeSystemFileType(launchPath);
		if (fileType == CafeTitleFileType::RPX || fileType == CafeTitleFileType::ELF)
		{
			CafeSystem::PREPARE_STATUS_CODE r = CafeSystem::PrepareForegroundTitleFromStandaloneRPX(launchPath);
			if (r != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
			{
				return ERROR_UNKNOWN;
			}
		}
		else if (launchTitle.GetInvalidReason() == TitleInfo::InvalidReason::NO_DISC_KEY)
		{
			return ERROR_NO_DISC_KEY;
		}
		else if (launchTitle.GetInvalidReason() == TitleInfo::InvalidReason::NO_TITLE_TIK)
		{
			return ERROR_NO_TITLE_TIK;
		}
		else
		{
			return ERROR_UNKNOWN;
		}
	}

	return SUCCESSFUL;
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_launchTitle([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	CafeSystem::LaunchForegroundTitle();
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_pauseTitle([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	CafeSystem::PauseTitle();
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_resumeTitle([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	CafeSystem::ResumeTitle();
}
