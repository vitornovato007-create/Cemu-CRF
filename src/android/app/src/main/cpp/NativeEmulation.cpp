#include "JNIUtils.h"
#include "AndroidAudio.h"
#include "AndroidEmulatedController.h"
#include "AndroidFilesystemCallbacks.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/CafeSystem.h"
#include "Cemu/GuiSystem/GuiSystem.h"
#include "GameTitleLoader.h"
#include "input/ControllerFactory.h"
#include "input/InputManager.h"
#include "input/api/Android/AndroidController.h"
#include "input/api/Android/AndroidControllerProvider.h"
#include "config/ActiveSettings.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "Cemu/Logging/CemuLogging.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <map>
#include <thread>
#include <vector>

// forward declaration from main.cpp
void CemuCommonInit();

namespace NativeEmulation
{
    class AndroidLogCapture final : public LogCallbacks
    {
    public:
        AndroidLogCapture()
        {
            // Start only after every member (including the drop counter) has
            // completed initialization.
            m_writer = std::thread(&AndroidLogCapture::WriterLoop, this);
        }

        ~AndroidLogCapture()
        {
            {
                std::lock_guard lock(m_queueMutex);
                m_stopping = true;
            }
            m_queueCondition.notify_all();
            if (m_writer.joinable())
                m_writer.join();

            std::lock_guard lock(m_mutex);
            if (m_file.is_open())
            {
                m_file.flush();
                m_file.close();
            }
        }

        // Keep the emulation/logging thread away from disk I/O. The callback only
        // formats a bounded line and performs a short, non-blocking queue insert.
        void Log(std::string_view filter, std::string_view message) override
        {
            try
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
                if (line.size() > kMaxLineBytes)
                {
                    line.resize(kMaxLineBytes - 64);
                    line.append("\n[diagnostic line truncated]\n");
                }

                std::unique_lock lock(m_queueMutex, std::try_to_lock);
                if (!lock.owns_lock() || m_stopping)
                {
                    ++m_droppedMessages;
                    return;
                }

                while (!m_queue.empty() &&
                    (m_queue.size() >= kMaxQueuedLines || m_queueBytes + line.size() > kMaxQueuedBytes))
                {
                    m_queueBytes -= m_queue.front().size();
                    m_queue.pop_front();
                    ++m_droppedMessages;
                }

                if (line.size() > kMaxQueuedBytes)
                {
                    ++m_droppedMessages;
                    return;
                }

                m_queue.emplace_back(std::move(line));
                m_queueBytes += m_queue.back().size();
                lock.unlock();
                m_queueCondition.notify_one();
            }
            catch (...)
            {
                // Diagnostics must never propagate an exception into emulation.
                ++m_droppedMessages;
            }
        }

        void Log(std::string_view filter, std::wstring_view message) override
        {
            try
            {
                Log(filter, boost::nowide::narrow(message));
            }
            catch (...)
            {
                ++m_droppedMessages;
            }
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
            // Export is a user-initiated operation, so it may wait briefly for
            // the writer. The emulation callback itself never waits for this.
            {
                std::unique_lock queueLock(m_queueMutex);
                m_queueDrained.wait_for(queueLock, std::chrono::seconds(2), [this]() {
                    return m_queue.empty() && !m_writerBusy;
                });
            }

            std::lock_guard lock(m_mutex);
            if (m_file.is_open())
                m_file.flush();

            std::string result;
            result.reserve(std::min<size_t>(m_fileBytes + 1024, kMaxBytes * 2 + 8192));
            AppendFileIfPresent(m_previousPath, result);
            AppendFileIfPresent(m_persistentPath, result);

            if (!result.empty())
            {
                if (!m_unpersistedLines.empty())
                {
                    result.append("\n===== Recent entries not confirmed on disk =====\n");
                    for (const auto& line : m_unpersistedLines)
                        result.append(line);
                }
                if (m_unpersistedWasTrimmed)
                    result.append("\n[WARNING] Some entries that could not be persisted were discarded at the 32 MiB recovery limit.\n");
                if (!m_fileError.empty())
                    result.append("\n[DIAGNOSTIC CAPTURE WARNING] ").append(m_fileError).append("\n");
            }
            else
            {
                result.reserve(m_totalBytes + 512);
                result.append("Persistent log file unavailable; showing bounded in-memory capture only.\n");
                if (!m_fileError.empty())
                    result.append("[DIAGNOSTIC CAPTURE WARNING] ").append(m_fileError).append("\n");
                if (m_wasTrimmed)
                    result.append("[WARNING] Older in-memory log entries were discarded at the 32 MiB limit.\n");
                for (const auto& line : m_lines)
                    result.append(line);
            }

            const uint64 dropped = m_droppedMessages.load();
            if (dropped != 0)
                result.append(fmt::format("\n[WARNING] {} diagnostic messages were dropped to keep logging non-blocking and bounded.\n", dropped));

            // Include entries still queued if the writer did not drain within
            // the export wait window.
            {
                std::lock_guard queueLock(m_queueMutex);
                if (!m_queue.empty())
                {
                    result.append("\n===== Entries still queued at export time =====\n");
                    for (const auto& line : m_queue)
                        result.append(line);
                }
            }
            return result;
        }

    private:
        static constexpr size_t kMaxBytes = 32 * 1024 * 1024;
        static constexpr size_t kMaxLineBytes = 256 * 1024;
        static constexpr size_t kMaxQueuedBytes = 4 * 1024 * 1024;
        static constexpr size_t kMaxQueuedLines = 4096;
        static const std::chrono::steady_clock::time_point s_startTime;

        // m_mutex protects the in-memory ring and the persistent file.
        std::mutex m_mutex;
        std::deque<std::string> m_lines;
        fs::path m_persistentPath;
        fs::path m_previousPath;
        std::ofstream m_file;
        size_t m_totalBytes = 0;
        size_t m_unpersistedBytes = 0;
        std::deque<std::string> m_unpersistedLines;
        uint64 m_fileBytes = 0;
        bool m_wasTrimmed = false;
        bool m_unpersistedWasTrimmed = false;
        std::string m_fileError;

        // Separate bounded queue: the callback never performs file operations.
        std::mutex m_queueMutex;
        std::condition_variable m_queueCondition;
        std::condition_variable m_queueDrained;
        std::deque<std::string> m_queue;
        size_t m_queueBytes = 0;
        bool m_writerBusy = false;
        bool m_stopping = false;
        std::thread m_writer;
        std::atomic<uint64> m_droppedMessages{ 0 };

        void WriterLoop()
        {
            for (;;)
            {
                std::deque<std::string> batch;
                {
                    std::unique_lock lock(m_queueMutex);
                    m_queueCondition.wait(lock, [this]() { return m_stopping || !m_queue.empty(); });
                    if (m_queue.empty() && m_stopping)
                        break;

                    m_writerBusy = true;
                    batch.swap(m_queue);
                    m_queueBytes = 0;
                }

                try
                {
                    PersistBatch(batch);
                }
                catch (...)
                {
                    m_droppedMessages.fetch_add(batch.size());
                }

                {
                    std::lock_guard lock(m_queueMutex);
                    m_writerBusy = false;
                    if (m_queue.empty())
                        m_queueDrained.notify_all();
                }
            }

            std::lock_guard lock(m_mutex);
            if (m_file.is_open())
                m_file.flush();
        }

        void PersistBatch(const std::deque<std::string>& batch)
        {
            std::lock_guard lock(m_mutex);
            std::vector<std::string> writtenThisBatch;
            for (const auto& line : batch)
            {
                m_lines.emplace_back(line);
                m_totalBytes += line.size();
                while (m_totalBytes > kMaxBytes && !m_lines.empty())
                {
                    m_totalBytes -= m_lines.front().size();
                    m_lines.pop_front();
                    m_wasTrimmed = true;
                }

                bool persisted = false;
                if (m_file.is_open())
                {
                    RotateIfNeeded(line.size());
                    if (m_file.is_open())
                    {
                        m_file.write(line.data(), static_cast<std::streamsize>(line.size()));
                        if (m_file)
                        {
                            m_fileBytes += line.size();
                            persisted = true;
                            writtenThisBatch.emplace_back(line);
                        }
                        else
                        {
                            m_fileError = "write failed";
                        }
                    }
                }

                if (!persisted)
                {
                    m_unpersistedLines.emplace_back(line);
                    m_unpersistedBytes += line.size();
                    while (m_unpersistedBytes > kMaxBytes && !m_unpersistedLines.empty())
                    {
                        m_unpersistedBytes -= m_unpersistedLines.front().size();
                        m_unpersistedLines.pop_front();
                        m_unpersistedWasTrimmed = true;
                    }
                }
            }

            // One flush per batch instead of one flush per log message.
            if (m_file.is_open())
            {
                m_file.flush();
                if (!m_file)
                {
                    m_fileError = "batch flush failed; batch copied to in-memory recovery buffer";
                    // A successful write() is not durable until flush succeeds.
                    // Keep a recovery copy so an export never silently trusts a
                    // batch whose final flush failed.
                    for (const auto& line : writtenThisBatch)
                    {
                        m_unpersistedLines.emplace_back(line);
                        m_unpersistedBytes += line.size();
                        while (m_unpersistedBytes > kMaxBytes && !m_unpersistedLines.empty())
                        {
                            m_unpersistedBytes -= m_unpersistedLines.front().size();
                            m_unpersistedLines.pop_front();
                            m_unpersistedWasTrimmed = true;
                        }
                    }
                }
            }
        }

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
            if (m_file)
                m_fileBytes = marker.size();
            else
                m_fileError = "failed to write log rotation marker";
        }
    };

    const std::chrono::steady_clock::time_point AndroidLogCapture::s_startTime = std::chrono::steady_clock::now();

    AndroidLogCapture s_androidLogCapture;

    static void AppendTextureCacheSnapshot(std::string& report)
    {
        report.append("Texture lifecycle counters (process lifetime; independent of Vulkan Validation):\n");
        const auto lifecycle = LatteTexture_GetLifecycleDiagnostics();
        report.append(fmt::format("  Texture objects constructed: {}\n", lifecycle.constructedTotal));
        report.append(fmt::format("  Texture objects destroyed: {}\n", lifecycle.destroyedTotal));
        report.append(fmt::format("  Texture objects currently alive: {}\n", lifecycle.currentlyAlive));
        report.append("  Note: constructed/destroyed count LatteTexture object lifetimes, not GPU uploads or successful image allocations.\n\n");

        const auto loadStats = LatteTextureLoader_GetDiagnostics();
        report.append("Texture decode/upload counters (process lifetime):\n");
        report.append(fmt::format("  Texture slice load attempts: {}\n", loadStats.sliceLoadAttempts));
        report.append(fmt::format("  Decoder unavailable: {}\n", loadStats.decoderUnavailable));
        report.append(fmt::format("  Decoder calls completed: {}\n", loadStats.decodeCalls));
        report.append(fmt::format("  Decoded output bytes: {}\n", loadStats.decodedBytes));
        report.append(fmt::format("  Upload-buffer bytes requested: {}\n", loadStats.uploadBufferBytesRequested));
        report.append(fmt::format("  Upload-buffer allocation failures: {}\n", loadStats.uploadBufferAllocationFailures));
        report.append(fmt::format("  Renderer texture upload calls: {}\n", loadStats.rendererUploadCalls));
        report.append(fmt::format("  Bytes passed to renderer texture uploads: {}\n\n", loadStats.rendererUploadBytes));

        uint64 droppedTextureEvents = 0;
        const auto recentTextureEvents = LatteTextureLoader_GetRecentDiagnosticEvents(droppedTextureEvents);
        report.append("Recent texture loading failures (bounded ring, newest entries retained):\n");
        report.append(fmt::format("  Events overwritten or dropped: {}\n", droppedTextureEvents));
        if (recentTextureEvents.empty())
        {
            report.append("  No decoder or upload-buffer failures recorded in this process.\n");
        }
        else
        {
            for (const auto& event : recentTextureEvents)
            {
                const char* eventName = event.type == LatteTextureDiagnosticEventType::DecoderUnavailable
                    ? "decoder unavailable" : "upload buffer allocation failed";
                report.append(fmt::format("  {}: GX2=0x{:04x}, {}x{}, slice={}, mip={}, depth={}, bytes={}\n",
                    eventName, event.format, event.width, event.height, event.slice, event.mip, event.isDepth ? "yes" : "no", event.bytes));
            }
        }
        report.append("\n");

        report.append("Texture cache snapshot (current live cache; not cumulative load totals):\n");
        const auto textures = LatteTexture_QueryCacheInfo();
        if (textures.empty())
        {
            report.append("  No texture snapshot available (emulation may be idle or the render-thread query timed out).\n\n");
            return;
        }

        size_t depthCount = 0;
        size_t colorCount = 0;
        size_t gpuUpdatedCount = 0;
        size_t totalViews = 0;
        size_t totalMipLevels = 0;
        std::map<uint32, size_t> formatCounts;
        for (const auto& texture : textures)
        {
            if (texture.isDepth)
                ++depthCount;
            else
                ++colorCount;
            if (texture.isUpdatedOnGPU)
                ++gpuUpdatedCount;
            totalViews += texture.views.size() + 1;
            totalMipLevels += std::max(texture.mipLevels, 0);
            ++formatCounts[(uint32)texture.format];
        }

        report.append(fmt::format("  Live texture objects: {}\n", textures.size()));
        report.append(fmt::format("  Color textures: {}\n", colorCount));
        report.append(fmt::format("  Depth textures: {}\n", depthCount));
        report.append(fmt::format("  GPU-updated textures: {}\n", gpuUpdatedCount));
        report.append(fmt::format("  Texture views (including base views): {}\n", totalViews));
        report.append(fmt::format("  Sum of mip levels across texture objects: {}\n", totalMipLevels));
        report.append("  Objects by GX2 format (numeric enum):\n");
        for (const auto& [format, count] : formatCounts)
            report.append(fmt::format("    0x{:04x}: {}\n", format, count));
        report.append("\n");
    }

    std::string GetVulkanDiagnosticLog()
    {
        std::string report;
        report.reserve(1024 * 1024);
        report.append("Cemu Android diagnostic report\n");
        report.append("===============================\n");
        report.append("Capture source: Cemu native LogCallbacks (not Android-wide Logcat).\n");
        report.append("Persistent log: app-private vulkan-diagnostic.log with one rotated .previous segment.\n");
        report.append("Each active/previous segment is bounded to 32 MiB; export combines both when available.\n");
        report.append("Vulkan validation UI preference currently enabled: ");
        report.append(cemuLog_isLoggingEnabled(LogType::VulkanValidation) ? "yes (requested for next renderer initialization)\n" : "no\n");
        const auto validation = VulkanRenderer_GetValidationDiagnostics();
        report.append("Last Vulkan renderer initialization attempted: ");
        report.append(validation.initializationAttempted ? "yes\n" : "no\n");
        report.append("Validation layer status: ");
        if (!validation.initializationAttempted)
            report.append("unknown (renderer has not initialized in this process)\n");
        else if (!validation.validationRequested)
            report.append("not requested\n");
        else
            report.append(validation.validationLayerEnabled ? "loaded\n" : "not loaded\n");
        report.append(fmt::format("VK_EXT_debug_utils available: {}\n", validation.debugUtilsAvailable ? "yes" : "no"));
        report.append(fmt::format("Vulkan debug messenger created: {}\n", validation.debugMessengerCreated ? "yes" : "no"));
        if (validation.validationRequested)
            report.append(fmt::format("Debug messenger VkResult: {}\n\n", validation.debugMessengerResult));
        else
            report.append("\n");
        if (auto* renderer = VulkanRenderer::GetInstance())
        {
            if (auto* memoryManager = renderer->GetMemoryManager())
            {
                const auto memory = memoryManager->GetDiagnostics();
                report.append("Vulkan memory diagnostics (current allocator snapshot + process-lifetime counters):\n");
                report.append(fmt::format("  Texture heap count: {}\n", memory.textureHeapCount));
                report.append(fmt::format("  Texture heap allocated/reserved: {}/{} bytes\n", memory.textureAllocatedBytes, memory.textureHeapBytes));
                report.append(fmt::format("  Staging allocator used/free: {}/{} bytes\n", memory.stagingBufferBytes - memory.stagingBufferFreeBytes, memory.stagingBufferFreeBytes));
                report.append(fmt::format("  Index allocator used/free: {}/{} bytes\n", memory.indexBufferBytes - memory.indexBufferFreeBytes, memory.indexBufferFreeBytes));
                report.append(fmt::format("  Vertex-stride allocator used/free: {}/{} bytes\n", memory.strideBufferBytes - memory.strideBufferFreeBytes, memory.strideBufferFreeBytes));
                report.append(fmt::format("  Buffer create attempts/success/failure: {}/{}/{}\n", memory.bufferCreateAttempts, memory.bufferCreateSuccesses, memory.bufferCreateFailures));
                report.append(fmt::format("  Image allocation attempts/success/failure: {}/{}/{}\n", memory.imageAllocationAttempts, memory.imageAllocationSuccesses, memory.imageAllocationFailures));
                report.append(fmt::format("  Image allocation bytes (cumulative successful requests): {}\n\n", memory.imageAllocationBytes));
            }
        }
        else
        {
            report.append("Vulkan memory diagnostics: renderer not initialized in this process.\n\n");
        }
        AppendTextureCacheSnapshot(report);
        report.append(s_androidLogCapture.Snapshot());
        if (report.empty() || report.back() != '\n')
            report.push_back('\n');
        return report;
    }


	void initializeAudioDevices()
	{
		auto& config = g_config.data();
		if (!config.tv_device.empty())
			AndroidAudio::createAudioDevice(IAudioAPI::AudioAPI::Cubeb, config.tv_channels, config.tv_volume, true);

		if (!config.pad_device.empty())
			AndroidAudio::createAudioDevice(IAudioAPI::AudioAPI::Cubeb, config.pad_channels, config.pad_volume, false);
	}

	void createCemuDirectories()
	{
		std::wstring mlc = ActiveSettings::GetMlcPath().generic_wstring();

		// create sys/usr folder in mlc01
		const auto sysFolder = fs::path(mlc).append(L"sys");
		fs::create_directories(sysFolder);

		const auto usrFolder = fs::path(mlc).append(L"usr");
		fs::create_directories(usrFolder);
		fs::create_directories(fs::path(usrFolder).append("title/00050000")); // base
		fs::create_directories(fs::path(usrFolder).append("title/0005000c")); // dlc
		fs::create_directories(fs::path(usrFolder).append("title/0005000e")); // update

		// Mii Maker save folders {0x500101004A000, 0x500101004A100, 0x500101004A200},
		fs::create_directories(fs::path(mlc).append(L"usr/save/00050010/1004a000/user/common/db"));
		fs::create_directories(fs::path(mlc).append(L"usr/save/00050010/1004a100/user/common/db"));
		fs::create_directories(fs::path(mlc).append(L"usr/save/00050010/1004a200/user/common/db"));

		// lang files
		auto langDir = fs::path(mlc).append(L"sys/title/0005001b/1005c000/content");
		fs::create_directories(langDir);

		auto langFile = fs::path(langDir).append("language.txt");
		if (!fs::exists(langFile))
		{
			std::ofstream file(langFile);
			if (file.is_open())
			{
				const char* langStrings[] = {"ja", "en", "fr", "de", "it", "es", "zh", "ko", "nl", "pt", "ru", "zh"};
				for (const char* lang : langStrings)
					file << fmt::format(R"("{}",)", lang) << std::endl;

				file.flush();
				file.close();
			}
		}

		auto countryFile = fs::path(langDir).append("country.txt");
		if (!fs::exists(countryFile))
		{
			std::ofstream file(countryFile);
			for (sint32 i = 0; i < 201; i++)
			{
				const char* countryCode = NCrypto::GetCountryAsString(i);
				if (boost::iequals(countryCode, "NN"))
					file << "NULL," << std::endl;
				else
					file << fmt::format(R"("{}",)", countryCode) << std::endl;
			}
			file.flush();
			file.close();
		}

		// cemu directories
		const auto controllerProfileFolder = ActiveSettings::GetConfigPath(L"controllerProfiles").generic_wstring();
		if (!fs::exists(controllerProfileFolder))
			fs::create_directories(controllerProfileFolder);

		const auto memorySearcherFolder = ActiveSettings::GetUserDataPath(L"memorySearcher").generic_wstring();
		if (!fs::exists(memorySearcherFolder))
			fs::create_directories(memorySearcherFolder);
	}
	enum StartGameResult : sint32
	{
		SUCCESSFUL = 0,
		ERROR_GAME_BASE_FILES_NOT_FOUND = 1,
		ERROR_NO_DISC_KEY = 2,
		ERROR_NO_TITLE_TIK = 3,
		ERROR_UNKNOWN = 4,
	};
	StartGameResult startGame(const fs::path& launchPath)
	{
		TitleInfo launchTitle{launchPath};
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
		CafeSystem::LaunchForegroundTitle();
		return SUCCESSFUL;
	}
} // namespace NativeEmulation

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_setReplaceTVWithPadView([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jboolean swapped)
{
	// Emulate pressing the TAB key for showing DRC instead of TV
	GuiSystem::getWindowInfo().set_keystate(GuiSystem::PlatformKeyCodes::TAB, swapped);
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_initializeEmulation([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	FilesystemAndroid::setFilesystemCallbacks(std::make_shared<AndroidFilesystemCallbacks>());
	g_config.SetFilename(ActiveSettings::GetConfigPath("settings.xml").generic_wstring());
	NativeEmulation::createCemuDirectories();
	NetworkConfig::LoadOnce();
	ActiveSettings::Init();
	NativeEmulation::s_androidLogCapture.StartPersistentCapture(ActiveSettings::GetUserDataPath("vulkan-diagnostic.log"));
	cemuLog_registerLogCallbacks(&NativeEmulation::s_androidLogCapture);
	LatteOverlay_init();
	CemuCommonInit();
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_initializeRenderer(JNIEnv* env, [[maybe_unused]] jclass clazz, jobject j_testSurface)
{
	InitializeGlobalVulkan();
	using ANativewindow_Ptr = std::unique_ptr<ANativeWindow, decltype(&ANativeWindow_release)>;
	JNIUtils::handleNativeException(env, [&]() {
		cemu_assert_debug(j_testSurface != nullptr);
		ANativewindow_Ptr testSurface(ANativeWindow_fromSurface(env, j_testSurface), &ANativeWindow_release);
		GuiSystem::getWindowInfo().window_main.surface = testSurface.get();
		g_renderer = std::make_unique<VulkanRenderer>();
		GuiSystem::getWindowInfo().window_main.surface = nullptr;
	});
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_setDPI([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jfloat dpi)
{
	auto& windowInfo = GuiSystem::getWindowInfo();
	windowInfo.dpi_scale = windowInfo.pad_dpi_scale = dpi;
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_clearSurface([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jboolean is_main_canvas)
{
	if (!is_main_canvas)
	{
		auto renderer = static_cast<VulkanRenderer*>(g_renderer.get());
		if (renderer)
			renderer->StopUsingPadAndWait();
	}
}
extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_recreateRenderSurface([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jboolean is_main_canvas)
{
	// TODO
}

extern "C" [[maybe_unused]] JNIEXPORT jboolean JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_supportsLoadingCustomDriver([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	return SupportsLoadingCustomDriver();
}


extern "C" [[maybe_unused]] JNIEXPORT jstring JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_getVulkanDiagnosticLog(JNIEnv* env, [[maybe_unused]] jclass clazz)
{
    return JNIUtils::toJString(env, NativeEmulation::GetVulkanDiagnosticLog());
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_setVulkanValidationEnabled([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jboolean enabled)
{
    uint64 flags = s_loggingFlagMask;
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
Java_info_cemu_cemu_nativeinterface_NativeEmulation_setSurface(JNIEnv* env, [[maybe_unused]] jclass clazz, jobject surface, jboolean is_main_canvas)
{
	JNIUtils::handleNativeException(env, [&]() {
		cemu_assert_debug(surface != nullptr);
		auto& windowHandleInfo = is_main_canvas ? GuiSystem::getWindowInfo().canvas_main : GuiSystem::getWindowInfo().canvas_pad;
		if (windowHandleInfo.surface)
		{
			ANativeWindow_release(static_cast<ANativeWindow*>(windowHandleInfo.surface));
			windowHandleInfo.surface = nullptr;
		}
		windowHandleInfo.surface = ANativeWindow_fromSurface(env, surface);
		int width, height;
		if (is_main_canvas)
			GuiSystem::getWindowPhysSize(width, height);
		else
			GuiSystem::getPadWindowPhysSize(width, height);
		VulkanRenderer::GetInstance()->InitializeSurface({width, height}, is_main_canvas);
	});
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_setSurfaceSize([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jint width, jint height, jboolean is_main_canvas)
{
	auto& windowInfo = GuiSystem::getWindowInfo();
	if (is_main_canvas)
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
Java_info_cemu_cemu_nativeinterface_NativeEmulation_shutdownTitle([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	cemuLog_log(LogType::Force, "Android: requested return to main menu; shutting down foreground title");
	CafeSystem::ShutdownTitle();
	cemuLog_log(LogType::Force, "Android: foreground title shutdown completed; persistent Cemu logging remains active");
}

extern "C" [[maybe_unused]] JNIEXPORT jint JNICALL
Java_info_cemu_cemu_nativeinterface_NativeEmulation_startGame([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jstring launchPath)
{
	GuiSystem::getWindowInfo().set_keystates_up();
	NativeEmulation::initializeAudioDevices();
	return NativeEmulation::startGame(JNIUtils::toString(env, launchPath));
}
