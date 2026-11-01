#include "FileStream_saf.h"
#include "FilesystemAndroid.h"

#include <algorithm>
#include <limits>

FileStreamSAF* FileStreamSAF::OpenFile(const fs::path& path)
{
	auto fileStream = new FileStreamSAF(path);
	if (fileStream->m_isValid)
		return fileStream;

	delete fileStream;
	return nullptr;
}

void FileStreamSAF::SetPosition(uint64 pos)
{
	if (!m_isValid)
		return;

	// A short read sets eofbit/failbit. Clear it before seeking so a later
	// random-access read (common when loading archives) can recover.
	m_stream.clear();
	m_stream.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
}

uint64 FileStreamSAF::GetSize()
{
	if (!m_isValid)
		return 0;

	// Preserve the current offset and recover from a previous EOF/short read.
	m_stream.clear();
	auto currentPos = m_stream.tellg();
	if (currentPos == std::streampos(-1))
		currentPos = std::streampos(0);

	m_stream.clear();
	m_stream.seekg(0, std::ios::end);
	if (!m_stream)
	{
		m_stream.clear();
		m_stream.seekg(currentPos, std::ios::beg);
		return 0;
	}

	auto fileSize = m_stream.tellg();
	m_stream.clear();
	m_stream.seekg(currentPos, std::ios::beg);
	if (fileSize == std::streampos(-1))
		return 0;

	return static_cast<uint64>(fileSize);
}

bool FileStreamSAF::SetEndOfFile()
{
	return true;
}

void FileStreamSAF::extract(std::vector<uint8>& data)
{
	if (!m_isValid)
		return;

	const uint64 fileSize = GetSize();
	if (fileSize > static_cast<uint64>(std::numeric_limits<size_t>::max()))
		return;

	SetPosition(0);
	data.resize(static_cast<size_t>(fileSize));
	uint64 totalRead = 0;
	while (totalRead < fileSize)
	{
		const uint32 chunkSize = static_cast<uint32>(std::min<uint64>(fileSize - totalRead, 1024 * 1024));
		const uint32 bytesRead = readData(data.data() + totalRead, chunkSize);
		if (bytesRead == 0)
		{
			data.resize(static_cast<size_t>(totalRead));
			break;
		}
		totalRead += bytesRead;
	}
}

uint32 FileStreamSAF::readData(void* data, uint32 length)
{
	if (!m_isValid || length == 0)
		return 0;

	m_stream.read(static_cast<char*>(data), static_cast<std::streamsize>(length));
	const auto bytesRead = m_stream.gcount();
	return bytesRead > 0 ? static_cast<uint32>(bytesRead) : 0;
}

bool FileStreamSAF::readU64(uint64& v)
{
	return readData(&v, sizeof(uint64)) == sizeof(uint64);
}

bool FileStreamSAF::readU32(uint32& v)
{
	return readData(&v, sizeof(uint32)) == sizeof(uint32);
}

bool FileStreamSAF::readU8(uint8& v)
{
	return readData(&v, sizeof(uint8)) == sizeof(uint8);
}

bool FileStreamSAF::readLine(std::string& line)
{
	line.clear();
	uint8 c;
	bool isEOF = true;
	while (readU8(c))
	{
		isEOF = false;
		if (c == '\\r')
			continue;
		if (c == '\\n')
			break;
		line.push_back(static_cast<char>(c));
	}
	return !isEOF;
}

sint32 FileStreamSAF::writeData(const void* data, sint32 length)
{
	throw std::runtime_error("write operation not supported");
}

void FileStreamSAF::writeU64(uint64 v)
{
	throw std::runtime_error("write operation not supported");
}

void FileStreamSAF::writeU32(uint32 v)
{
	throw std::runtime_error("write operation not supported");
}

void FileStreamSAF::writeU8(uint8 v)
{
	throw std::runtime_error("write operation not supported");
}

void FileStreamSAF::writeStringFmt(const char* format, ...)
{
	throw std::runtime_error("write operation not supported");
}

void FileStreamSAF::writeString(const char* str)
{
	throw std::runtime_error("write operation not supported");
}

void FileStreamSAF::writeLine(const char* str)
{
	throw std::runtime_error("write operation not supported");
}

void FileStreamSAF::Flush()
{
	throw std::runtime_error("write operation not supported");
}

FileStreamSAF::FileStreamSAF(const fs::path& uri) : m_stream(FilesystemAndroid::OpenContentUri(uri))
{
	m_isValid = m_stream.IsValid();
}
